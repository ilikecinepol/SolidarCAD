#include "ui/MainWindow.h"
#include "ui/PreviewUpdateCoordinator.h"
#include "ui/AssemblyTreeModel.h"

#include "model/ExtrudeFeature.h"
#include "model/ComponentGraphAnalysis.h"
#include "model/SketchExtrudeBuilder.h"
#include "ui/interaction/ContextActionResolver.h"
#include "model/ChamferFeature.h"
#include "model/ExtrudeOperationDetector.h"
#include "model/FilletFeature.h"
#include "model/ShellFeature.h"
#include "model/DraftFeature.h"
#include "ui/ToolParametersPanel.h"
#include "ui/ToolIcon.h"
#include "ui/PartDesignToolHelp.h"
#include "ui/PartDesignHistory.h"
#include "ui/PartDesignErrorLocalization.h"
#include "ui/application/PartDesignCommitPolicy.h"
#include "ui/FeatureUiRegistry.h"
#include "ui/HistoryTimelineWidget.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProductEnvelopeV5Relocation.h"
#include "project/LinkedPartSourceResolver.h"
#include "ui/application/LinkedPartLifecycleService.h"
#include "ui/application/ModelWorkspaceProjectionSuccessor.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>
#include <QDockWidget>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QIcon>
#include <QImageReader>
#include <QInputDialog>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QButtonGroup>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QItemSelectionModel>
#include <QAction>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QKeySequence>
#include <QMenuBar>
#include <QMenu>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPointer>
#include <QPrintDialog>
#include <QPrinter>
#include <QStatusBar>
#include <QStyle>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSet>
#include <QShortcut>
#include <QToolButton>
#include <QTimer>
#include <QVariant>
#include <QUuid>
#include <limits>

#include "drawing/EskdRenderer.h"
#include "drawing/DrawingSource.h"
#include "sketch/SketchRibbon.h"
#include "ui/DrawingSheetView.h"
#include "ui/ModelRibbon.h"
#include "ui/SettingsWidget.h"
#include "ui/SketchCanvas.h"
#include "ui/Viewport.h"

namespace solidar {

namespace {

QString componentInstanceTreeKey(const ComponentInstanceAddress& instance) {
  QString key = QString::fromLatin1("%1:%2")
                    .arg(instance.rootDefinitionId.value())
                    .arg(instance.definitionId.value());
  if (instance.occurrencePath) {
    for (const OccurrenceId id : instance.occurrencePath->entries())
      key += QString::fromLatin1(":%1").arg(id.value());
  }
  return key;
}

QString stableComponentTreeKey(const ComponentTreeAddressValue& address) {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ComponentTreeRootAddress>)
          return QStringLiteral("root/") +
                 componentInstanceTreeKey(value.instance);
        else if constexpr (std::is_same_v<T,
                                          ComponentTreeOccurrenceAddress>)
          return QStringLiteral("occurrence/") +
                 componentInstanceTreeKey(value.instance);
        else if constexpr (std::is_same_v<T, ComponentTreeGroupAddress>)
          return QString::fromLatin1("group/%1/%2")
              .arg(componentInstanceTreeKey(value.instance))
              .arg(static_cast<int>(value.group));
        else if constexpr (std::is_same_v<T,
                                          ComponentTreeOriginPlaneAddress>)
          return QString::fromLatin1("plane/%1/%2")
              .arg(componentInstanceTreeKey(value.instance))
              .arg(static_cast<int>(value.plane));
        else if constexpr (std::is_same_v<T, ComponentTreeBodyAddress>)
          return QString::fromLatin1("body/%1/%2")
              .arg(componentInstanceTreeKey(value.body.component))
              .arg(value.body.bodyId);
        else if constexpr (std::is_same_v<T, ComponentTreeSketchAddress>)
          return QString::fromLatin1("sketch/%1/%2")
              .arg(componentInstanceTreeKey(value.instance))
              .arg(value.sketchId);
        else if constexpr (std::is_same_v<
                               T, ComponentTreeReferenceImageAddress>)
          return QString::fromLatin1("image/%1/%2")
              .arg(componentInstanceTreeKey(value.instance))
              .arg(value.imageId);
        else if constexpr (std::is_same_v<T, ComponentTreeDrawingAddress>)
          return QString::fromLatin1("drawing/%1/%2")
              .arg(componentInstanceTreeKey(value.instance))
              .arg(value.drawingId.value());
        else if constexpr (std::is_same_v<T,
                                          ComponentTreeUnusedSectionAddress>)
          return QString::fromLatin1("unused/%1")
              .arg(value.rootDefinitionId.value());
        else
          return QString::fromLatin1("unused-definition/%1/%2")
              .arg(value.rootDefinitionId.value())
              .arg(value.definitionId.value());
      },
      address);
}

QSet<QString> captureExpandedComponentTree(
    const QTreeView* view, const ComponentTreeModel* model) noexcept {
  QSet<QString> expanded;
  if (!view || !model || view->model() != model) return expanded;
  try {
    const auto visit = [&](const auto& self, const QModelIndex& parent) -> void {
      for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (view->isExpanded(index)) {
          const auto address = model->addressForIndex(index);
          if (address)
            expanded.insert(
                stableComponentTreeKey(address->get().value()));
        }
        self(self, index);
      }
    };
    visit(visit, {});
  } catch (...) {
    expanded.clear();
  }
  return expanded;
}

void restoreExpandedComponentTree(
    QTreeView* view, const ComponentTreeModel* model,
    const QSet<QString>& expanded) noexcept {
  if (!view || !model || view->model() != model) return;
  try {
    const auto visit = [&](const auto& self, const QModelIndex& parent) -> void {
      for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        const auto address = model->addressForIndex(index);
        view->setExpanded(
            index,
            address && expanded.contains(
                           stableComponentTreeKey(address->get().value())));
        self(self, index);
      }
    };
    visit(visit, {});
  } catch (...) {
  }
}

class ComponentEditProjectionSuccessor final
    : public ProductDocumentSuccessor {
 public:
  ComponentEditProjectionSuccessor(
      ProductPresentationEpoch epoch, AssemblyMeshProfile meshProfile,
      ComponentWorkspaceSemanticState semanticState,
      ComponentViewportSessionFailurePoint sessionFailurePoint,
      ComponentTreeModelFailurePoint treeFailurePoint) noexcept
      : epoch_(epoch),
        meshProfile_(meshProfile),
        semanticState_(std::move(semanticState)),
        sessionFailurePoint_(sessionFailurePoint),
        treeFailurePoint_(treeFailurePoint) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument& candidate,
      ProductDocumentSuccessorImpact impact) noexcept override {
    if (impact != ProductDocumentSuccessorImpact::DefinitionProjection &&
        impact != ProductDocumentSuccessorImpact::TreeOnly)
      return ProductDocumentSuccessorStatus::Rejected;
    auto prepared = ComponentProjectionPreparer::prepare(
        candidate, epoch_, meshProfile_, semanticState_, sessionFailurePoint_,
        treeFailurePoint_);
    if (!prepared)
      return prepared.status == ComponentWorkspacePrepareStatus::ResourceFailed
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    projection_ = std::move(prepared.projection);
    return ProductDocumentSuccessorStatus::Prepared;
  }

  [[nodiscard]] std::unique_ptr<PreparedComponentProjection> release()
      noexcept {
    return std::move(projection_);
  }

 private:
  ProductPresentationEpoch epoch_;
  AssemblyMeshProfile meshProfile_;
  ComponentWorkspaceSemanticState semanticState_;
  ComponentViewportSessionFailurePoint sessionFailurePoint_;
  ComponentTreeModelFailurePoint treeFailurePoint_;
  std::unique_ptr<PreparedComponentProjection> projection_;
};

struct ComponentWorkspaceProjection final {
  std::vector<ComponentWorkspaceInstanceKey> instances;
  std::vector<ComponentWorkspaceBodyKey> bodies;
  std::vector<ComponentWorkspaceDrawingKey> drawings;
};

void canonicalizeComponentWorkspaceProjection(
    ComponentWorkspaceProjection& projection) {
  std::sort(projection.instances.begin(), projection.instances.end(),
            componentWorkspaceInstanceKeyLess);
  std::sort(projection.bodies.begin(), projection.bodies.end(),
            componentWorkspaceBodyKeyLess);
  std::sort(projection.drawings.begin(), projection.drawings.end(),
            componentWorkspaceDrawingKeyLess);
}

std::optional<ComponentWorkspaceProjection> captureComponentWorkspaceProjection(
    const ProductDocument& product) noexcept {
  try {
    ComponentWorkspaceProjection result;
    if (const auto* part = product.rootPartDefinition()) {
      const ComponentWorkspaceInstanceKey root{product.rootDefinitionId(),
                                               std::nullopt};
      result.instances.push_back(root);
      result.bodies.reserve(part->document().bodies().size());
      for (const auto& body : part->document().bodies())
        result.bodies.push_back({root, body.id()});
      canonicalizeComponentWorkspaceProjection(result);
      return result;
    }
    if (!product.rootComponentDefinition()) return std::nullopt;
    const auto traversal =
        expandComponentGraph(product.definitions(), product.rootDefinitionId());
    if (!traversal) return std::nullopt;
    result.instances.reserve(traversal.visits.size());
    for (const auto& visit : traversal.visits) {
      const ComponentWorkspaceInstanceKey instance{visit.definitionId,
                                                   visit.path};
      result.instances.push_back(instance);
      const auto* definition =
          product.definitions().findComponent(visit.definitionId);
      const auto* document = definition ? &definition->document() : nullptr;
      if (!document) return std::nullopt;
      for (const auto& body : document->bodies())
        result.bodies.push_back({instance, body.id()});
      for (const auto& drawing : definition->drawings())
        result.drawings.push_back({instance, drawing.id()});
    }
    canonicalizeComponentWorkspaceProjection(result);
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<DefinitionId> lowestFreeComponentDefinitionId(
    const ProductDocument& product) noexcept {
  try {
    std::uint64_t candidate = 1;
    for (const DefinitionId id : product.definitions().definitionIds()) {
      if (id.value() < candidate) continue;
      if (id.value() != candidate) return DefinitionId{candidate};
      if (candidate == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
      ++candidate;
    }
    return DefinitionId{candidate};
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<OccurrenceId> lowestFreeComponentOccurrenceId(
    const ProductDocument& product) noexcept {
  const auto* root = product.rootComponentDefinition();
  if (!root) return OccurrenceId{1};
  std::uint64_t candidate = 1;
  for (const auto& [id, occurrence] : root->occurrences()) {
    static_cast<void>(occurrence);
    if (id.value() < candidate) continue;
    if (id.value() != candidate) return OccurrenceId{candidate};
    if (candidate == std::numeric_limits<std::uint64_t>::max())
      return std::nullopt;
    ++candidate;
  }
  return OccurrenceId{candidate};
}

QString componentExtractionDiagnostic(ComponentExtractionStatus status,
                                      const std::string& detail = {}) {
  QString message;
  switch (status) {
    case ComponentExtractionStatus::CrossDefinitionDependency:
      message = QString::fromUtf8(
          "Тело связано с объектами, которые должны остаться в активном компоненте");
      break;
    case ComponentExtractionStatus::BodyMissing:
      message = QString::fromUtf8("Выбранное тело больше не существует");
      break;
    case ComponentExtractionStatus::InvalidDependencyGraph:
      message = QString::fromUtf8("Граф зависимостей модели повреждён");
      break;
    case ComponentExtractionStatus::ResourceFailure:
      message = QString::fromUtf8("Недостаточно ресурсов для создания компонента");
      break;
    default:
      message = QString::fromUtf8("Не удалось создать компонент из выбранного тела");
      break;
  }
  if (!detail.empty())
    message += QString::fromUtf8(": ") + QString::fromStdString(detail);
  return message;
}

QString componentPlacementDiagnostic(ComponentPlacementStatus status) {
  switch (status) {
    case ComponentPlacementStatus::Grounded:
      return QString::fromUtf8("Зафиксированный компонент нельзя перемещать");
    case ComponentPlacementStatus::Suppressed:
      return QString::fromUtf8("Подавленный компонент нельзя перемещать");
    case ComponentPlacementStatus::EffectivelyHidden:
      return QString::fromUtf8("Скрытый компонент нельзя перемещать");
    case ComponentPlacementStatus::NoChange:
      return QString::fromUtf8("Положение компонента не изменилось");
    case ComponentPlacementStatus::OccurrenceMissing:
    case ComponentPlacementStatus::TargetDefinitionMismatch:
    case ComponentPlacementStatus::OwnerStateMismatch:
    case ComponentPlacementStatus::PlacementMismatch:
      return QString::fromUtf8(
          "Компонент изменился; повторите перетаскивание");
    case ComponentPlacementStatus::ResourceFailure:
    case ComponentPlacementStatus::SuccessorResourceFailure:
      return QString::fromUtf8(
          "Недостаточно ресурсов для перемещения компонента");
    default:
      return QString::fromUtf8("Не удалось переместить компонент");
  }
}

QString componentHomePlacementDiagnostic(ComponentHomePlacementStatus status) {
  switch (status) {
    case ComponentHomePlacementStatus::Grounded:
      return QString::fromUtf8(
          "Зафиксированный компонент нельзя вернуть в исходное положение");
    case ComponentHomePlacementStatus::Suppressed:
      return QString::fromUtf8(
          "Подавленный компонент нельзя вернуть в исходное положение");
    case ComponentHomePlacementStatus::EffectivelyHidden:
      return QString::fromUtf8(
          "Скрытый компонент нельзя вернуть в исходное положение");
    case ComponentHomePlacementStatus::NoChange:
      return QString::fromUtf8("Положение уже совпадает с исходным");
    case ComponentHomePlacementStatus::ResourceFailure:
    case ComponentHomePlacementStatus::SuccessorResourceFailure:
      return QString::fromUtf8(
          "Недостаточно ресурсов для изменения исходного положения");
    case ComponentHomePlacementStatus::OccurrenceMissing:
    case ComponentHomePlacementStatus::TargetDefinitionMismatch:
    case ComponentHomePlacementStatus::OwnerDefinitionMismatch:
    case ComponentHomePlacementStatus::OwnerStateMismatch:
    case ComponentHomePlacementStatus::PlacementMismatch:
    case ComponentHomePlacementStatus::HomePlacementMismatch:
      return QString::fromUtf8("Компонент изменился; выберите его повторно");
    default:
      return QString::fromUtf8("Не удалось изменить исходное положение");
  }
}

struct ResolvedComponentPlacementTarget final {
  DefinitionId ownerDefinitionId;
  DefinitionStateId ownerStateId;
  DefinitionId targetDefinitionId;
  RigidTransform3d placement;
  RigidTransform3d homePlacement;
  OccurrenceFlags flags;
  bool effectivelyVisible{true};
  bool effectivelySuppressed{};
};

const ComponentDefinition* resolveComponentDefinition(
    const ProductDocument& product,
    const ComponentInstanceAddress& address) noexcept {
  if (address.rootDefinitionId != product.rootDefinitionId() ||
      address.definitionId == kInvalidDefinitionId)
    return nullptr;
  const ComponentDefinition* definition = product.rootComponentDefinition();
  if (!definition) return nullptr;
  if (!address.occurrencePath) {
    return address.definitionId == definition->id() ? definition : nullptr;
  }
  const auto& entries = address.occurrencePath->entries();
  if (entries.empty()) return nullptr;
  for (const OccurrenceId occurrenceId : entries) {
    const ComponentOccurrence* occurrence = definition->find(occurrenceId);
    if (!occurrence) return nullptr;
    definition = product.definitions().findComponent(
        occurrence->targetDefinitionId());
    if (!definition) return nullptr;
  }
  return definition->id() == address.definitionId ? definition : nullptr;
}

const ComponentOccurrence* resolveComponentOccurrence(
    const ProductDocument& product, const OccurrencePath& path) noexcept {
  const ComponentDefinition* owner = product.rootComponentDefinition();
  if (!owner || path.entries().empty()) return nullptr;
  const ComponentOccurrence* occurrence = nullptr;
  for (std::size_t index = 0; index < path.entries().size(); ++index) {
    occurrence = owner->find(path.entries()[index]);
    if (!occurrence) return nullptr;
    if (index + 1 < path.entries().size()) {
      owner = product.definitions().findComponent(
          occurrence->targetDefinitionId());
      if (!owner) return nullptr;
    }
  }
  return occurrence;
}

bool drawingMatchesProjection(
    const ComponentDrawing& drawing,
    const ComponentTreeDrawingProjection& projection) noexcept {
  return drawing.id() == projection.id &&
         drawing.nameUtf8() == projection.nameUtf8 &&
         drawing.paper() == projection.paper &&
         drawing.orientation() == projection.orientation &&
         drawing.scale() == projection.scale &&
         drawing.sourceBodyIds() == projection.sourceBodyIds;
}

std::optional<ResolvedComponentPlacementTarget>
resolveComponentPlacementTarget(const ProductDocument& product,
                                const ComponentInstanceAddress& address)
    noexcept {
  if (address.rootDefinitionId != product.rootDefinitionId() ||
      !address.occurrencePath ||
      address.occurrencePath->entries().empty())
    return std::nullopt;
  const ComponentDefinition* owner = product.rootComponentDefinition();
  if (!owner) return std::nullopt;
  const auto& entries = address.occurrencePath->entries();
  bool effectivelyVisible = true;
  bool effectivelySuppressed = false;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto* occurrence = owner->find(entries[index]);
    if (!occurrence) return std::nullopt;
    effectivelyVisible = effectivelyVisible && occurrence->flags().visible;
    effectivelySuppressed =
        effectivelySuppressed || occurrence->flags().suppressed;
    if (index + 1 == entries.size()) {
      if (occurrence->targetDefinitionId() != address.definitionId)
        return std::nullopt;
      return ResolvedComponentPlacementTarget{
          owner->id(), owner->stateId(), occurrence->targetDefinitionId(),
          occurrence->placement(), occurrence->homePlacement(),
          occurrence->flags(), effectivelyVisible, effectivelySuppressed};
    }
    owner = product.definitions().findComponent(
        occurrence->targetDefinitionId());
    if (!owner) return std::nullopt;
  }
  return std::nullopt;
}

std::optional<AssemblyCommandSemanticState> copySemanticState(
    const AssemblyCommandSemanticState& state) noexcept {
  try {
    return state;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<AssemblyCommandSemanticState> occurrenceSemanticState(
    const OccurrencePath& path) noexcept {
  try {
    return AssemblyCommandSemanticState{{path}, {}};
  } catch (...) {
    return std::nullopt;
  }
}

QString normalizedSolidarPath(QString path) {
  if (!path.endsWith(QStringLiteral(".solidar"), Qt::CaseInsensitive))
    path += QStringLiteral(".solidar");
  return path;
}

bool isWritableAssemblyFormat(
    project::ProjectPersistenceFormat format) noexcept {
  return project::isWritableAssemblyPersistenceFormat(format);
}

bool isWritableComponentFormat(
    project::ProjectPersistenceFormat format) noexcept {
  return project::isWritableComponentPersistenceFormat(format);
}

bool componentProductHasDrawings(const ProductDocument& product) noexcept {
  try {
    for (const DefinitionId id : product.definitions().definitionIds()) {
      const auto* component = product.definitions().findComponent(id);
      if (component && !component->drawings().empty()) return true;
    }
  } catch (...) {
    // A conservative V11 choice is safe and prevents a lossy V10 save.
    return true;
  }
  return false;
}

project::ProjectPersistenceState componentPersistenceForSave(
    const ProductDocument& product,
    project::ProjectPersistenceState current) noexcept {
  if (current.format == project::ProjectPersistenceFormat::V12)
    return {project::ProjectPersistenceFormat::V12};
  try {
    for (const DefinitionId id : product.definitions().definitionIds()) {
      const auto* component = product.definitions().findComponent(id);
      if (!component) continue;
      for (const auto& [occurrenceId, occurrence] : component->occurrences()) {
        static_cast<void>(occurrenceId);
        if (occurrence.homePlacement() != occurrence.placement())
          return {project::ProjectPersistenceFormat::V12};
      }
    }
  } catch (...) {
    return {project::ProjectPersistenceFormat::V12};
  }
  if (current.format == project::ProjectPersistenceFormat::V11 ||
      componentProductHasDrawings(product))
    return {project::ProjectPersistenceFormat::V11};
  return {project::ProjectPersistenceFormat::V10};
}

struct AssemblyOccurrencePersistenceSeal final {
  OccurrenceId id;
  DefinitionId target;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags;
  std::string name;
  friend bool operator==(const AssemblyOccurrencePersistenceSeal&,
                         const AssemblyOccurrencePersistenceSeal&) = default;
};

struct AssemblyDefinitionPersistenceSeal final {
  DefinitionId id;
  std::vector<AssemblyOccurrencePersistenceSeal> occurrences;
  std::uint64_t registryRevision{};
  std::uint64_t registryFingerprint{};
  MateSolverOptions options;
  friend bool operator==(const AssemblyDefinitionPersistenceSeal&,
                         const AssemblyDefinitionPersistenceSeal&) = default;
};

struct AssemblyPersistenceSnapshot final {
  std::vector<std::pair<DefinitionId, DefinitionStateId>> partStates;
  std::vector<AssemblyDefinitionPersistenceSeal> assemblies;
  std::size_t historySize{};
  std::size_t historyCursor{};
  std::size_t retainedBytes{};
  bool canRedo{};
};

std::optional<AssemblyPersistenceSnapshot> captureAssemblyPersistenceSnapshot(
    const ProductDocument& product,
    const AssemblyCommandHistory& history) noexcept {
  try {
    AssemblyPersistenceSnapshot snapshot;
    for (const DefinitionId id : product.definitions().definitionIds()) {
      if (const auto* part = product.definitions().findPart(id)) {
        snapshot.partStates.emplace_back(id, part->stateId());
        continue;
      }
      const auto* assembly = product.definitions().findAssembly(id);
      if (!assembly) return std::nullopt;
      AssemblyDefinitionPersistenceSeal seal;
      seal.id = id;
      seal.registryRevision = assembly->mateRegistry().revision();
      seal.registryFingerprint = assembly->mateRegistry().fingerprint();
      seal.options = assembly->mateRegistry().options();
      for (const auto& [occurrenceId, occurrence] :
           assembly->occurrences())
        seal.occurrences.push_back(
            {occurrenceId, occurrence.targetDefinitionId(),
             occurrence.placement(), occurrence.flags(),
             occurrence.displayNameUtf8()});
      snapshot.assemblies.push_back(std::move(seal));
    }
    snapshot.historySize = history.size();
    snapshot.historyCursor = history.cursor();
    snapshot.retainedBytes = history.retainedBytes();
    snapshot.canRedo = history.canRedo();
    return snapshot;
  } catch (...) {
    return std::nullopt;
  }
}

bool pathsReferToSameFile(const QString& left, const QString& right) {
  if (left.isEmpty() || right.isEmpty()) return false;

#ifdef Q_OS_WIN
  constexpr auto comparison = Qt::CaseInsensitive;
#else
  constexpr auto comparison = Qt::CaseSensitive;
#endif
  const QFileInfo leftInfo(left);
  const QFileInfo rightInfo(right);
  const QString leftAbsolute = QDir::cleanPath(leftInfo.absoluteFilePath());
  const QString rightAbsolute = QDir::cleanPath(rightInfo.absoluteFilePath());
  if (leftAbsolute.compare(rightAbsolute, comparison) == 0) return true;

  const QString leftCanonical = leftInfo.canonicalFilePath();
  const QString rightCanonical = rightInfo.canonicalFilePath();
  return !leftCanonical.isEmpty() && !rightCanonical.isEmpty() &&
         leftCanonical.compare(rightCanonical, comparison) == 0;
}

ProductDocumentIdentity newProductIdentityForPath(const QString& path) {
  const QByteArray uuid = QUuid::createUuid().toRfc4122();
  ProjectId::Bytes bytes{};
  for (qsizetype index = 0;
       index < uuid.size() && index < static_cast<qsizetype>(bytes.size());
       ++index)
    bytes[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>(uuid.at(index));
  const QByteArray name = QFileInfo(path).completeBaseName().toUtf8();
  const QByteArray created =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8();
  return {ProjectId{bytes},
          {std::string(name.constData(), static_cast<std::size_t>(name.size())),
           std::string(created.constData(),
                       static_cast<std::size_t>(created.size()))}};
}

project::LinkedPartSourcePolicy linkedPolicyForAssemblyPath(
    const QString& path) {
  project::LinkedPartSourcePolicy policy;
  policy.projectRoot = QFileInfo(path).absolutePath();
#if defined(Q_OS_WIN)
  policy.pathSemantics = project::LinkedPathSemantics::Windows;
#else
  policy.pathSemantics = project::LinkedPathSemantics::Posix;
#endif
  return policy;
}

std::vector<std::byte> coldBytes(const QByteArray& bytes) {
  std::vector<std::byte> result(static_cast<std::size_t>(bytes.size()));
  if (!result.empty())
    std::memcpy(result.data(), bytes.constData(), result.size());
  return result;
}
template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

QString projectOpenErrorMessage(const ProjectApplicationStatus& status) {
  QString message;
  switch (status.code) {
    case ProjectApplicationErrorCode::InvalidProject:
      message = QString::fromUtf8(
          "Файл проекта повреждён или имеет недопустимый формат.");
      break;
    case ProjectApplicationErrorCode::UnsupportedProject:
      message = QString::fromUtf8(
          "Версия или возможности этого проекта пока не поддерживаются.");
      break;
    default:
      message = QString::fromUtf8("Не удалось открыть проект.");
      break;
  }
  if (!status.detail.isEmpty())
    message += QString::fromUtf8("\n\nТехнические сведения: ") + status.detail;
  return message;
}

const ShapeFeature* presentationOwner(const Body& body,
                                      const ShapeFeature::ShapePtr& shape) {
  if (!shape) return nullptr;
  for (auto feature = body.features().rbegin();
       feature != body.features().rend(); ++feature)
    if ((*feature)->lastValidShape().get() == shape.get()) return feature->get();
  return nullptr;
}

bool bodyConsumedByJoin(const Document& document, BodyId bodyId,
                        std::optional<FeatureId> excludedJoin = std::nullopt) {
  for (const auto& candidateBody : document.bodies())
    for (const auto& candidate : candidateBody.features())
      if (const auto* join =
              dynamic_cast<const JoinBodiesFeature*>(candidate.get()))
        if ((!excludedJoin || join->id() != *excludedJoin) &&
            (join->firstBodyId() == bodyId ||
            join->secondBodyId() == bodyId)
           )
          return true;
  return false;
}

void releaseUnconsumedJoinOperands(Document& document,
                                   const std::vector<BodyId>& operands) {
  for (const BodyId operandId : operands) {
    Body* operand = document.findBody(operandId);
    if (operand && !bodyConsumedByJoin(document, operandId))
      operand->setVisible(true);
  }
}

std::vector<BodyId> removedJoinOperandIds(
    const Document& before, const FeatureRemovalPlan& plan) {
  std::vector<BodyId> result;
  for (const FeatureId featureId : plan.featureIds)
    if (const auto* join = dynamic_cast<const JoinBodiesFeature*>(
            before.findFeature(featureId))) {
      result.push_back(join->firstBodyId());
      result.push_back(join->secondBodyId());
    }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

QString sketchPresentationLabel(const DocumentSketch& sketch) {
  if (sketch.support.type == SketchSupportType::Face)
    return QString::fromUtf8("Грань");
  const Vector3d normal = sketch.placement.normal();
  if (std::abs(normal.y) > 0.9) return QStringLiteral("XZ");
  if (std::abs(normal.x) > 0.9) return QStringLiteral("YZ");
  return QStringLiteral("XY");
}

// Maps stable technical model-layer errors to Russian user messages at the UI
// boundary. The model keeps technical English strings; only the UI translates.
}  // namespace

MainWindow::MainWindow(AppSettings& settings, QWidget* parent)
    : QMainWindow(parent),
      productDocument_(
          std::move(StagedApplicationDocument(Document{}, std::nullopt,
                                              std::nullopt))
              .releaseProductDocument()),
      settings_(settings) {
  assemblyMateController_ =
      std::make_unique<AssemblyMateToolController>();
  previewUpdates_ = std::make_unique<PreviewUpdateCoordinator>(this);
  buildUi();
  buildMenus();
  resize(1200, 760);
  setWindowTitle(QString::fromUtf8("СОЛИДАР3D — Скетчер ЕСКД [*]"));
}

MainWindow::~MainWindow() {
  // QMainWindow destroys its dock widgets after MainWindow's data members.
  // Destroying historyDock_ can toggle its standard action, so disconnect the
  // callback while productDocument_ and the other state it reads are alive.
  if (historyToggleAction_)
    QObject::disconnect(historyToggleAction_, nullptr, this, nullptr);
  // Drop queued lambdas while their captured UI/model state is still alive,
  // then tear down every ToolSession before Document member destruction.
  cancelAssemblyInsertBeforeReplacement();
  preparePartDesignForReplacement();
  detachDefinitionEditForReplacement();
  detachAssemblyInsertUi(true);
  clearAssemblyCommandHistoryForReplacement();
  QObject::disconnect(assemblyTreeSelectionConnection_);
  QObject::disconnect(componentTreeSelectionConnection_);
  if (assemblyTreeView_) assemblyTreeView_->setModel(nullptr);
  if (componentTreeView_) componentTreeView_->setModel(nullptr);
  if (viewport_) viewport_->clearComponentSession();
  if (viewport_) viewport_->clearAssemblySession();
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::OldWorkspaceDetached, false);
}

void MainWindow::applyActivePartDesignTool() {
  if (!activePartDesignUiContextAllowed()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Apply));
}

bool MainWindow::applyPartDesignBeginResult(
    PartDesignTransitionOutcome outcome, PartDesignToolKind kind) {
  if (!standalonePartProjectActive() && !componentDefinitionEditContext_ &&
      !(definitionEditContext_ && kind == PartDesignToolKind::Move))
    return false;
  PartDesignUiEffect effect;
  effect.transition = outcome;
  effect.state = partDesignCoordinator_.snapshot(kind, activePartDocument());
  if (outcome.effect == PartDesignTransitionEffect::Activated) {
    applyPartDesignUiEffect(std::move(effect));
    return true;
  }

  effect.clearPreview = true;
  effect.clearSelection = true;
  effect.hidePanel = true;
  applyPartDesignUiEffect(std::move(effect));
  mirrorPresentationBefore_.reset();
  if (outcome.failure.code != OperationFailureCode::None)
    statusBar()->showMessage(localizedPartDesignError(kind, outcome.failure),
                             4000);
  return false;
}

void MainWindow::applyPartDesignUiEffect(PartDesignUiEffect effect) {
  if (!activePartDesignUiContextAllowed() &&
      !(definitionEditContext_ &&
        effect.transition.previousTool == PartDesignToolKind::Move))
    return;
  if (!std::holds_alternative<std::monostate>(effect.commitRequest)) {
    std::visit(
        Overloaded{
            [](std::monostate) {},
            [this](PartDesignUiEffect::ExtrudeCommit) { acceptFaceExtrudeTool(); },
            [this](PartDesignUiEffect::RevolveCommit) { acceptRevolveTool(); },
            [this](PartDesignUiEffect::FilletCommit) { acceptFilletTool(); },
            [this](PartDesignUiEffect::ChamferCommit) { acceptChamferTool(); },
            [this](PartDesignUiEffect::JoinBodiesCommit) { acceptJoinBodiesTool(); },
            [this](PartDesignUiEffect::ShellCommit) { acceptShellTool(); },
            [this](PartDesignUiEffect::DraftCommit) { acceptDraftTool(); },
            [this](PartDesignUiEffect::MirrorCommit) { acceptMirrorTool(); },
            [this](PartDesignUiEffect::MoveCommit) { acceptMoveTool(); },
            [this](PartDesignUiEffect::LinearPatternCommit) {
              acceptLinearPatternTool();
            },
            [this](PartDesignUiEffect::CircularPatternCommit) {
              acceptCircularPatternTool();
            }},
        effect.commitRequest);
    return;
  }

  if (effect.refreshPreview || effect.transition.effect ==
                                   PartDesignTransitionEffect::ReselectionCancelled) {
    std::visit(
        Overloaded{
            [](std::monostate) {},
            [this](PartDesignUiEffect::ExtrudeCommit) { updateFaceExtrudeToolPreview(); },
            [this](PartDesignUiEffect::RevolveCommit) { updateRevolveToolPreview(); },
            [this](PartDesignUiEffect::FilletCommit) { updateFilletToolPreview(); },
            [this](PartDesignUiEffect::ChamferCommit) { updateChamferToolPreview(); },
            [this](PartDesignUiEffect::JoinBodiesCommit) { updateJoinBodiesToolPreview(); },
            [this](PartDesignUiEffect::ShellCommit) { updateShellToolPreview(); },
            [this](PartDesignUiEffect::DraftCommit) { updateDraftToolPreview(); },
            [this](PartDesignUiEffect::MirrorCommit) { updateMirrorToolPreview(); },
            [this](PartDesignUiEffect::MoveCommit) { updateMoveToolPreview(); },
            [this](PartDesignUiEffect::LinearPatternCommit) {
              updateLinearPatternToolPreview();
            },
            [this](PartDesignUiEffect::CircularPatternCommit) {
              updateCircularPatternToolPreview();
            }},
        effect.target);
  }

  if (effect.clearPreview) {
    invalidatePreviewUpdates();
    viewport_->resetToolInteraction();
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    viewport_->clearLegacyExtrusionPreview();
  }
  if (effect.clearSelection) {
    viewport_->setSelectedBodies({});
    viewport_->setSelectedBodyEdges({});
    viewport_->setSelectedBodyFaces({});
    if (!effect.preserveSelectionMode) {
      viewport_->setEdgeMultiSelectionMode(false);
      viewport_->setFaceMultiSelectionMode(false);
      viewport_->setSelectionFilter(SelectionFilter::Any);
    }
  }
  if (effect.beginInputSelection) viewport_->beginDraftFaceSelection();
  if (effect.hidePanel) {
    if (toolParametersDock_ && !toolParametersDock_->isHidden()) {
      if (effect.transition.previousTool == PartDesignToolKind::Extrude)
        viewport_->preserveWorldOriginOnNextResize();
      toolParametersDock_->hide();
    }
    if (revolveDock_) revolveDock_->hide();
    if (mirrorDock_) mirrorDock_->hide();
    if (moveDock_) moveDock_->hide();
    if (linearPatternDock_) linearPatternDock_->hide();
    if (circularPatternDock_) circularPatternDock_->hide();
    modelRibbon_->clearActiveTool();
    refreshBodyViewFromDocument();
    // A cancelled edit restores immediately.  An accepted edit must defer its
    // no-change selection restoration until after the feature/history views
    // have been rebuilt by the model-commit adapter.
    if (effect.transition.effect == PartDesignTransitionEffect::Cancelled) {
      restoreCancelledEditUiTransaction();
      if (mirrorPresentationBefore_)
        applyPresentationState(*mirrorPresentationBefore_);
      mirrorPresentationBefore_.reset();
    }
  }
  // Tool activation/cancellation changes the quiescence precondition used by
  // persistence commands. Keep their QAction state synchronized with the
  // coordinator transition itself, including nested UI event loops.
  updateUpgradeProjectFormatAction();
  updateCreateComponentFromBodyAvailability();
}

void MainWindow::openSettings() {
  // Shared settings control inside a modal dialog; switching the theme is a
  // presentation-only change that never touches the open Document.
  QDialog dialog(this);
  dialog.setWindowTitle(QString::fromUtf8("Настройки"));
  dialog.resize(460, 340);
  auto* layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(24, 24, 24, 24);
  auto* settingsWidget = new SettingsWidget(settings_, &dialog);
  layout->addWidget(settingsWidget);
  // The editor dialog must close on Cancel; the Home page leaves the signal
  // unconnected because there is no modal dialog to close.
  connect(settingsWidget, &SettingsWidget::cancelRequested, &dialog,
          &QDialog::reject);
  dialog.exec();
}

void MainWindow::updateSketchConstraintPanel() {
  if (!partProjectActive()) return;
  if (!sketchConstraintsList_ || !sketchCanvas_) return;

  const QSignalBlocker blocker(sketchConstraintsList_);
  sketchConstraintsList_->clear();

  const auto entries = sketchCanvas_->selectedConstraintPanelEntries();

  if (entries.empty()) {
    auto* item = new QTreeWidgetItem(sketchConstraintsList_);
    item->setText(0, QString::fromUtf8("Нет ограничений"));
    // Use the disabled palette role from ThemeManager so this placeholder
    // follows Light/Dark automatically.
    item->setFlags(Qt::NoItemFlags);
    return;
  }

  for (const auto& entry : entries) {
    auto* item = new QTreeWidgetItem(sketchConstraintsList_);
    item->setText(0, entry.description);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable |
                   Qt::ItemIsEnabled |
                   (entry.isDimension() ? Qt::ItemIsUserTristate
                                        : Qt::NoItemFlags));
    item->setCheckState(
        1, entry.mode == sketch::ConstraintMode::Driving
               ? Qt::Checked
               : entry.mode == sketch::ConstraintMode::Reference
                     ? Qt::PartiallyChecked
                     : Qt::Unchecked);

    item->setData(
        0, Qt::UserRole,
        QVariant::fromValue<qulonglong>(
            static_cast<qulonglong>(entry.constraintId)));

    item->setData(
        0, Qt::UserRole + 1,
        QVariant::fromValue<qulonglong>(
            entry.isDimension()
                ? static_cast<qulonglong>(entry.dimensionIndex)
                : std::numeric_limits<qulonglong>::max()));
  }
}
void MainWindow::buildMenus() {
  auto* menuHost = new QWidget(this);
  auto* layout = new QVBoxLayout(menuHost);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto* bar = new QMenuBar(menuHost);
  auto* fileMenu = bar->addMenu(QString::fromUtf8("Файл"));
  createAction_ = fileMenu->addAction(QString::fromUtf8("Создать"));
  createAction_->setObjectName(QStringLiteral("createProjectAction"));
  createAction_->setShortcut(QKeySequence::New);
  auto* openAction = fileMenu->addAction(QString::fromUtf8("Открыть"));
  openAction->setShortcut(QKeySequence::Open);
  saveAction_ = fileMenu->addAction(QString::fromUtf8("Сохранить"));
  saveAction_->setObjectName(QStringLiteral("saveProjectAction"));
  saveAction_->setShortcut(QKeySequence::Save);
  saveProjectAsAction_ =
      fileMenu->addAction(QString::fromUtf8("Сохранить как…"));
  saveProjectAsAction_->setObjectName(QStringLiteral("saveProjectAsAction"));
  saveProjectAsAction_->setShortcut(QKeySequence::SaveAs);
  upgradeProjectFormatAction_ = fileMenu->addAction(
      QString::fromUtf8("Обновить формат проекта до v4…"));
  upgradeProjectFormatAction_->setObjectName(
      QStringLiteral("upgradeProjectFormatAction"));
  convertToProductDocumentAction_ = fileMenu->addAction(
      QString::fromUtf8("Преобразовать в документ изделия…"));
  convertToProductDocumentAction_->setObjectName(
      QStringLiteral("convertToProductDocumentAction"));
  relocateProductDocumentAction_ = fileMenu->addAction(
      QString::fromUtf8("Переместить документ изделия…"));
  relocateProductDocumentAction_->setObjectName(
      QStringLiteral("relocateProductDocumentAction"));
  updateUpgradeProjectFormatAction();
  auto* importMenu = fileMenu->addMenu(QString::fromUtf8("Импорт"));
  importStepAction_ =
      importMenu->addAction(QStringLiteral("STEP (*.step *.stp)"));
  importStepAction_->setObjectName(QStringLiteral("importStepAction"));
  auto* exportMenu = fileMenu->addMenu(QString::fromUtf8("Экспорт"));
  exportStepAction_ =
      exportMenu->addAction(QStringLiteral("STEP (*.step *.stp)"));
  exportStepAction_->setObjectName(QStringLiteral("exportStepAction"));
  exportStlAction_ = exportMenu->addAction(QStringLiteral("STL (*.stl)"));
  exportStlAction_->setObjectName(QStringLiteral("exportStlAction"));

  auto* editMenu = bar->addMenu(QString::fromUtf8("Правка"));
  undoAction_ = editMenu->addAction(QString::fromUtf8("Отменить"));
  undoAction_->setObjectName(QStringLiteral("undoAction"));
  undoAction_->setShortcut(QKeySequence::Undo);
  undoAction_->setEnabled(false);
  redoAction_ = editMenu->addAction(QString::fromUtf8("Повторить"));
  redoAction_->setObjectName(QStringLiteral("redoAction"));
  redoAction_->setShortcut(QKeySequence::Redo);
  redoAction_->setEnabled(false);
  auto* settingsAction = editMenu->addAction(QString::fromUtf8("Настройки…"));
  settingsAction->setObjectName("settingsAction");
  connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettings);
  editMenu->addSeparator();
  linkedPartReloadAction_ = editMenu->addAction(
      QString::fromUtf8("Обновить связанную деталь"));
  linkedPartReloadAction_->setObjectName(
      QStringLiteral("reloadLinkedPartAction"));
  linkedPartRepairAction_ = editMenu->addAction(
      QString::fromUtf8("Исправить ссылку…"));
  linkedPartRepairAction_->setObjectName(
      QStringLiteral("repairLinkedPartAction"));
  linkedPartReloadAction_->setEnabled(false);
  linkedPartRepairAction_->setEnabled(false);
  connect(linkedPartReloadAction_, &QAction::triggered, this,
          &MainWindow::reloadSelectedLinkedPart);
  connect(linkedPartRepairAction_, &QAction::triggered, this,
          &MainWindow::repairSelectedLinkedPart);

  // QDockWidget's close button only hides a panel.  Expose the standard
  // toggle actions so a closed construction tree or history panel always has
  // an obvious, state-synchronised way back.
  auto* viewMenu = bar->addMenu(QString::fromUtf8("Вид"));
  if (modelTreeDock_) {
    QAction* action = modelTreeDock_->toggleViewAction();
    action->setText(QString::fromUtf8("Дерево построений"));
    action->setObjectName(QStringLiteral("toggleModelTreeAction"));
    viewMenu->addAction(action);
  }
  if (historyDock_) {
    historyToggleAction_ = historyDock_->toggleViewAction();
    historyToggleAction_->setText(QString::fromUtf8("История построений"));
    historyToggleAction_->setObjectName(QStringLiteral("toggleHistoryAction"));
    viewMenu->addAction(historyToggleAction_);
    connect(historyToggleAction_, &QAction::toggled, this,
            [this](bool checked) {
              if (partProjectActive() || !checked) return;
              const QSignalBlocker blocker(historyToggleAction_);
              historyToggleAction_->setChecked(false);
              historyDock_->hide();
            });
  }

  viewMenu->addSeparator();
  auto* themeMenu = viewMenu->addMenu(QString::fromUtf8("Тема"));
  themeMenu->setObjectName(QStringLiteral("themeMenu"));
  auto* themeGroup = new QActionGroup(themeMenu);
  themeGroup->setExclusive(true);
  const auto addThemeAction = [themeMenu, themeGroup](
                                  const QString& text,
                                  const QString& objectName) {
    QAction* action = themeMenu->addAction(text);
    action->setObjectName(objectName);
    action->setCheckable(true);
    themeGroup->addAction(action);
    return action;
  };
  QAction* systemTheme =
      addThemeAction(QString::fromUtf8("Системная"),
                     QStringLiteral("themeSystemAction"));
  QAction* lightTheme =
      addThemeAction(QString::fromUtf8("Светлая"),
                     QStringLiteral("themeLightAction"));
  QAction* darkTheme =
      addThemeAction(QString::fromUtf8("Тёмная"),
                     QStringLiteral("themeDarkAction"));

  const auto syncThemeActions = [this, systemTheme, lightTheme, darkTheme] {
    const QSignalBlocker systemBlocker(systemTheme);
    const QSignalBlocker lightBlocker(lightTheme);
    const QSignalBlocker darkBlocker(darkTheme);
    systemTheme->setChecked(settings_.theme() == AppTheme::System);
    lightTheme->setChecked(settings_.theme() == AppTheme::Light);
    darkTheme->setChecked(settings_.theme() == AppTheme::Dark);
  };
  syncThemeActions();
  connect(systemTheme, &QAction::triggered, this,
          [this] { settings_.setTheme(AppTheme::System); });
  connect(lightTheme, &QAction::triggered, this,
          [this] { settings_.setTheme(AppTheme::Light); });
  connect(darkTheme, &QAction::triggered, this,
          [this] { settings_.setTheme(AppTheme::Dark); });
  connect(&settings_, &AppSettings::themeChanged, this,
          [syncThemeActions](AppTheme) { syncThemeActions(); });

  layout->addWidget(bar);
  ribbonStack_->setParent(menuHost);
  layout->addWidget(ribbonStack_);
  setMenuWidget(menuHost);

  connect(createAction_, &QAction::triggered, this, &MainWindow::createProject);
  connect(openAction, &QAction::triggered, this, &MainWindow::openProject);
  connect(saveAction_, &QAction::triggered, this, &MainWindow::saveProject);
  connect(saveProjectAsAction_, &QAction::triggered, this,
          &MainWindow::saveProjectAs);
  connect(upgradeProjectFormatAction_, &QAction::triggered, this,
          &MainWindow::upgradeProjectFormat);
  connect(convertToProductDocumentAction_, &QAction::triggered, this,
          &MainWindow::convertToProductDocument);
  connect(relocateProductDocumentAction_, &QAction::triggered, this,
          &MainWindow::relocateProductDocument);
  connect(importStepAction_, &QAction::triggered, this, &MainWindow::importStep);
  connect(exportStepAction_, &QAction::triggered, this, &MainWindow::exportStep);
  connect(exportStlAction_, &QAction::triggered, this, &MainWindow::exportStl);
  connect(undoAction_, &QAction::triggered, this, &MainWindow::undoLastAction);
  connect(redoAction_, &QAction::triggered, this, &MainWindow::redoLastAction);
  connect(sketchCanvas_, &SketchCanvas::undoAvailable, this,
          [this](bool) {
            if (!partProjectActive()) return;
            updateUndoAvailability();
          });
  connect(sketchCanvas_, &SketchCanvas::redoAvailable, this,
          [this](bool) {
            if (!partProjectActive()) return;
            updateUndoAvailability();
          });
}

bool MainWindow::commitDefinitionCandidate(Document candidate) {
  if (!definitionEditContext_ || assemblyCommandHistory_.isBusy()) return false;
  const DefinitionId target = definitionEditContext_->targetDefinitionId;
  const Document* current = productDocument_.partDocument(target);
  if (!current) return false;

  // Ported commands validate a detached candidate before entering the
  // authoritative ProductDocument transaction. The transaction recomputes
  // again after applying the cold delta, so this is not a substitute for the
  // guarded commit boundary.
  if (!candidate.recompute()) return false;

  DocumentDelta delta;
  try {
    delta = DocumentDelta::create(*current, candidate);
  } catch (...) {
    return false;
  }
  if (delta.empty()) return true;

  DefinitionEditColdCommand command{
      {definitionEditContext_->productIncarnation,
       definitionEditContext_->rootDefinitionId, target,
       productDocument_.definitionStateId(target)},
      std::move(delta),
      {{}, target},
      {{}, target}};
  std::unique_ptr<PreparedAssemblyProjection> successor;
  std::unique_ptr<PreparedAssemblyProjection> visibilitySuccessor;
  std::unique_ptr<PreparedAssemblyProjection> groundingSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> suppressionSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> duplicateSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> removeSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> definitionSuccessor;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex treeSelectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, successor, visibilitySuccessor, groundingSuccessor,
      suppressionSuccessor, duplicateSuccessor, removeSuccessor,
      definitionSuccessor, treeOnly, treeSelectionIndex, preparedSelection,
      AssemblyCommandSemanticState{{}, target});
  const bool previousModified = isWindowModified();
  const auto result = assemblyCommandHistory_.executeDefinitionEdit(
      productDocument_, std::move(command), prepare);
  if (result.status != AssemblyCommandHistoryStatus::Committed) {
    setWindowModified(previousModified);
    updateUndoAvailability();
    return false;
  }
  installAssemblyDefinitionProjection(std::move(definitionSuccessor),
                                      treeSelectionIndex);
  definitionEditContext_->stateAtEntry =
      productDocument_.definitionStateId(target);
  syncSketchPresentationFromDocument();
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
  finishEditUiTransaction();
  updateUndoAvailability();
  return true;
}

bool MainWindow::pushModelTransition(
    Document previous, HistorySelectionState previousSelection,
    std::optional<HistorySelectionState> committedAfter) {
  if (definitionEditContext_) {
    // Legacy Part commands mutate their supplied Document before reaching
    // this hook. They are deliberately disabled in definition-edit context;
    // fail closed if one is invoked programmatically and restore the exact
    // authoritative state. Ported commands must use commitDefinitionCandidate
    // and never touch the live embedded Document first.
    Q_ASSERT_X(false, "MainWindow::pushModelTransition",
               "legacy Part mutation reached embedded definition context");
    updateUndoAvailability();
    return false;
  }
  if (modelHistory_.isApplying()) return true;
  const bool previousModified = isWindowModified();
  HistorySelectionState beforeSelection = editUiTransaction_
      ? editUiTransaction_->before
      : std::move(previousSelection);
  HistorySelectionState afterSelection = committedAfter
      ? std::move(*committedAfter)
      : captureHistorySelection();
  const auto result = modelHistory_.recordTransition(
      mutableStandaloneDocument(), std::move(previous), std::move(beforeSelection),
      std::move(afterSelection));

  if (result.status == HistoryCommitStatus::NoChange) {
    // The caller still owns tool-specific teardown. Defer restoration until
    // that teardown has finished so its selection clearing cannot overwrite
    // the exact pre-edit state.
    if (editUiTransaction_)
      pendingNoChangeUiRestore_ = *editUiTransaction_;
    updateUndoAvailability();
    return true;
  }

  if (result.status == HistoryCommitStatus::Rejected) {
    // Callers publish/tear down transient UI only after acceptance.  The
    // service has restored the Document, so leaving the active editor/session
    // untouched keeps a rejected operation immediately retryable.
    setWindowModified(previousModified);
    try { updateUndoAvailability(); } catch (...) {}
    const QString detail = QString::fromStdString(result.error);
    try {
      statusBar()->showMessage(
          detail.contains(QStringLiteral("budget"), Qt::CaseInsensitive)
              ? QString::fromUtf8("Операция отменена: превышен лимит истории")
              : QString::fromUtf8("Операция отменена: история не создана (%1)")
                    .arg(detail),
          5000);
    } catch (...) {}
    return false;
  }

  if (componentDefinitionEditContext_ &&
      !commitActiveComponentDocument()) {
    const auto rollback = modelHistory_.undo(
        mutableStandaloneDocument(), captureHistorySelection());
    static_cast<void>(rollback);
    modelHistory_.clear(true);
    setWindowModified(previousModified);
    statusBar()->showMessage(
        QString::fromUtf8("Изменение компонента не применено"), 5000);
    updateUndoAvailability();
    return false;
  }

  setWindowModified(!modelHistory_.isAtSavepoint());
  finishEditUiTransaction();
  updateUndoAvailability();
  return true;
}

bool MainWindow::commitActiveComponentDocument() {
  if (!componentDefinitionEditContext_ || !componentEditDocument_)
    return false;
  if (!viewport_ ||
      componentPresentationEpochClock_ ==
          std::numeric_limits<std::uint64_t>::max())
    return false;
  Document candidate;
  try {
    candidate = *componentEditDocument_;
  } catch (...) {
    return false;
  }
  const DefinitionId target =
      componentDefinitionEditContext_->targetDefinitionId;
  const ComponentWorkspaceInstanceKey instance{
      target, componentDefinitionEditContext_->occurrencePath};
  const ProductPresentationEpoch nextEpoch{
      componentPresentationEpochClock_ + 1};
  const AssemblyMeshProfile profile{
      viewport_->meshQuality() == ViewportMeshQuality::High
          ? AssemblyMeshQuality::High
          : AssemblyMeshQuality::Normal};
  ComponentEditProjectionSuccessor successor{
      nextEpoch, profile,
      ComponentWorkspaceSemanticState{instance, instance, std::nullopt,
                                      "active component edit"},
      componentPreparationFailurePoint_, componentTreeFailurePoint_};
  const auto result = productDocument_.applyDefinitionEdit(
      {componentDefinitionEditContext_->productIncarnation,
       componentDefinitionEditContext_->rootDefinitionId, target,
       productDocument_.definitionStateId(target)},
      candidate, successor);
  if (result.status != DefinitionEditStatus::Committed) return false;
  componentDefinitionEditContext_->stateAtEntry = result.committedStateId;
  auto projection = successor.release();
  if (!projection) std::terminate();
  installActiveComponentProjection(std::move(projection));
  return true;
}

void MainWindow::installActiveComponentProjection(
    std::unique_ptr<PreparedComponentProjection> projection) noexcept {
  if (!projection || !componentDefinitionEditContext_ ||
      !componentTreeView_ || !viewport_)
    std::terminate();

  const QSet<QString> expanded =
      captureExpandedComponentTree(componentTreeView_, componentTreeModel_);
  QObject::disconnect(componentTreeSelectionConnection_);
  componentTreeSelectionConnection_ = {};
  componentTreeView_->setModel(nullptr);
  delete componentTreeModel_;
  componentTreeModel_ = nullptr;

  auto session = projection->releaseSession();
  auto tree = projection->releaseTreeModel();
  if (!session || !session->scene() || !tree) std::terminate();
  componentPresentationEpochClock_ = projection->epoch().value();
  const bool rootAuthoring = rootComponentAuthoringActive();
  if (rootAuthoring)
    viewport_->replaceComponentSessionDuringEdit(std::move(session));
  else
    suspendedComponentSession_ = std::move(session);
  componentTreeModel_ = tree.release();
  componentTreeModel_->setParent(componentTreeView_);
  connectComponentTreeVisibilitySignals();
  componentTreeView_->setModel(componentTreeModel_);
  restoreExpandedComponentTree(componentTreeView_, componentTreeModel_,
                               expanded);
  componentTreeSelectionConnection_ = connect(
      componentTreeView_->selectionModel(),
      &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        QPointer<MainWindow> receiver(this);
        selectComponentTreeIndex(current);
        if (!receiver) return;
        updateCreateComponentFromBodyAvailability();
      });

  const ComponentInstanceAddress active{
      projection->epoch(), productDocument_.rootDefinitionId(),
      componentDefinitionEditContext_->targetDefinitionId,
      componentDefinitionEditContext_->occurrencePath};
  const auto scene = rootAuthoring
                         ? viewport_->componentSession()->scene()
                         : suspendedComponentSession_->scene();
  viewport_->setComponentEditContext(scene, active);
  updateComponentTreePresentation();
}

void MainWindow::completeModelTransitionUi() {
  if (!pendingNoChangeUiRestore_) return;
  const auto transaction = std::move(*pendingNoChangeUiRestore_);
  pendingNoChangeUiRestore_.reset();
  restoreHistorySelection(transaction.before);
  applyPresentationState(transaction.before.presentation);
  setWindowModified(transaction.modified);
  finishEditUiTransaction();
  updateUndoAvailability();
}

bool MainWindow::pushPresentationTransition(
    HistorySelectionState before, HistorySelectionState after) {
  if (definitionEditContext_) {
    applyPresentationState(after.presentation);
    updateUndoAvailability();
    return true;
  }
  if (modelHistory_.isApplying()) return true;
  Document previous = activePartDocument();
  const bool previousModified = isWindowModified();
  const auto result = modelHistory_.recordTransition(
      mutableStandaloneDocument(), std::move(previous), std::move(before), std::move(after));
  if (result.status == HistoryCommitStatus::Rejected) {
    applyPresentationState(result.state.presentation);
    setWindowModified(previousModified);
    updateUndoAvailability();
    return false;
  }
  if (result.status == HistoryCommitStatus::Accepted)
    setWindowModified(!modelHistory_.isAtSavepoint());
  updateUndoAvailability();
  return true;
}

MainWindow::HistorySelectionState MainWindow::captureHistorySelection() const {
  HistorySelectionState state;
  state.sketchViewIds.reserve(sketchViews_.size());
  for (const auto& view : sketchViews_)
    state.sketchViewIds.push_back(view.documentSketchId);
  state.historyPosition = historyPosition_;
  state.atEnd = isHistoryAtEnd();
  state.historicalLegacyExtrusionSourceSketchId =
      historicalLegacyExtrusionSourceSketchId_;
  const QPointF bodyPosition = viewport_->bodyPosition();
  state.presentation.bodyPositionX = bodyPosition.x();
  state.presentation.bodyPositionY = bodyPosition.y();
  state.presentation.originVisible = viewport_->originVisible();
  state.presentation.sketchVisible = viewport_->sketchVisible();
  for (int plane = 0; plane < 3; ++plane)
    state.presentation.basePlanesVisible[static_cast<std::size_t>(plane)] =
        viewport_->basePlaneVisible(plane);
  state.presentation.sketchVisibilities.reserve(sketchViews_.size());
  for (std::size_t index = 0; index < sketchViews_.size(); ++index)
    state.presentation.sketchVisibilities.push_back(
        viewport_->sketchVisible(index) ? 1U : 0U);
  if (!state.atEnd && state.historyPosition > 0 &&
      state.historyPosition <= static_cast<int>(historySteps_.size())) {
    const auto& step = historySteps_[static_cast<std::size_t>(
        state.historyPosition - 1)];
    state.historyBodyId = step.bodyId;
    state.historyFeatureId = step.featureId;
    state.historySketchId = step.sketchId;
  }
  // A history location is represented by one stable selection domain only.
  state.faces = viewport_->selectedBodyFaces();
  if (state.faces.empty()) state.edges = viewport_->selectedBodyEdges();
  if (state.faces.empty() && state.edges.empty())
    state.bodies = viewport_->selectedBodies();
  return state;
}

MainWindow::HistorySelectionState
MainWindow::captureCommittedEndState() const {
  HistorySelectionState state = captureHistorySelection();
  state.bodies.clear();
  state.edges.clear();
  state.faces.clear();
  state.historyBodyId = kInvalidBodyId;
  state.historyFeatureId = kInvalidFeatureId;
  state.historySketchId = kInvalidSketchId;
  state.historyPosition = std::numeric_limits<int>::max();
  state.atEnd = true;
  if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Mirror &&
      mirrorPresentationBefore_)
    state.presentation.basePlanesVisible =
        mirrorPresentationBefore_->basePlanesVisible;
  return state;
}

void MainWindow::restoreHistorySelection(const HistorySelectionState& state) {
  historicalLegacyExtrusionSourceSketchId_ =
      state.historicalLegacyExtrusionSourceSketchId;
  int desiredPosition = state.atEnd
      ? static_cast<int>(historySteps_.size())
      : std::clamp(state.historyPosition, 0,
                   static_cast<int>(historySteps_.size()));
  if (!state.atEnd && (state.historyBodyId != kInvalidBodyId ||
                       state.historyFeatureId != kInvalidFeatureId ||
                       state.historySketchId != kInvalidSketchId)) {
    const auto found = std::find_if(
        historySteps_.begin(), historySteps_.end(),
        [&state](const HistoryStep& step) {
          return step.bodyId == state.historyBodyId &&
                 step.featureId == state.historyFeatureId &&
                 step.sketchId == state.historySketchId;
        });
    if (found != historySteps_.end())
      desiredPosition = static_cast<int>(
          std::distance(historySteps_.begin(), found) + 1);
  }
  applyHistoryPosition(desiredPosition);
  viewport_->setSelectedBodies({});
  viewport_->setSelectedBodyEdges({});
  viewport_->setSelectedBodyFaces({});
  if (!state.faces.empty()) {
    std::vector<FaceReference> valid;
    for (const auto& face : state.faces)
      if (activePartDocument().findBody(face.bodyId()) &&
          activePartDocument().findFeature(face.featureId())) valid.push_back(face);
    viewport_->setSelectedBodyFaces(valid);
  } else if (!state.edges.empty()) {
    std::vector<EdgeReference> valid;
    for (const auto& edge : state.edges)
      if (activePartDocument().findBody(edge.bodyId()) &&
          activePartDocument().findFeature(edge.featureId())) valid.push_back(edge);
    viewport_->setSelectedBodyEdges(valid);
  }
  else if (!state.bodies.empty()) {
    std::vector<BodyId> valid;
    for (const auto id : state.bodies)
      if (activePartDocument().findBody(id)) valid.push_back(id);
    viewport_->setSelectedBodies(valid);
  }
}

void MainWindow::applyPresentationState(
    const EditorPresentationState& state) {
  // Presentation history is applied as one typed adapter transaction.  Block
  // the feature tree while both views are updated so programmatic Undo/Redo
  // cannot record another command or leave the checkbox one click behind.
  const QSignalBlocker treeBlocker(featureTree_);
  viewport_->setBodyPosition(
      QPointF(state.bodyPositionX, state.bodyPositionY));
  viewport_->setOriginVisible(state.originVisible);
  viewport_->setSketchVisible(state.sketchVisible);
  for (int plane = 0; plane < 3; ++plane)
    viewport_->setBasePlaneVisible(
        plane, state.basePlanesVisible[static_cast<std::size_t>(plane)]);
  for (std::size_t index = 0;
       index < state.sketchVisibilities.size() &&
       index < sketchViews_.size(); ++index)
    viewport_->setSketchVisible(index,
                                state.sketchVisibilities[index] != 0U);

  const auto items = featureTree_->findItems(
      QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
  for (QTreeWidgetItem* item : items) {
    const int kind = item->data(0, Qt::UserRole).toInt();
    std::optional<bool> visible;
    if (kind == 1)
      visible = state.originVisible;
    else if (kind == 2)
      visible = state.sketchVisible;
    else if (kind >= 10 && kind <= 12)
      visible = state.basePlanesVisible[static_cast<std::size_t>(kind - 10)];
    else if (kind >= 20) {
      const auto index = static_cast<std::size_t>(kind - 20);
      if (index < state.sketchVisibilities.size())
        visible = state.sketchVisibilities[index] != 0U;
    }
    if (visible)
      item->setCheckState(0, *visible ? Qt::Checked : Qt::Unchecked);
  }
}

void MainWindow::synchronizeCommittedState(
    const HistorySelectionState& state) {
  sketchViews_.clear();
  sketchViews_.reserve(state.sketchViewIds.size());
  for (const SketchId id : state.sketchViewIds)
    if (activePartDocument().findSketch(id)) sketchViews_.push_back({id});
  historicalLegacyExtrusionSourceSketchId_ =
      state.historicalLegacyExtrusionSourceSketchId;
  viewport_->setBox(activePartDocument().box());
  syncSketchPresentationFromDocument();
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  restoreHistorySelection(state);
  // History/model rebuilds derive temporary visibility for the selected
  // history step.  The committed typed presentation is authoritative and is
  // therefore applied last to both viewport and tree.
  applyPresentationState(state.presentation);
  setWindowModified(!modelHistory_.isAtSavepoint());
}

void MainWindow::restoreCancelledEditUiTransaction() {
  if (!editUiTransaction_ || suppressEditCancelRestore_) return;
  const auto transaction = std::move(*editUiTransaction_);
  editUiTransaction_.reset();
  pendingNoChangeUiRestore_.reset();
  restoreHistorySelection(transaction.before);
  applyPresentationState(transaction.before.presentation);
  setWindowModified(transaction.modified);
}

void MainWindow::finishEditUiTransaction() noexcept {
  editUiTransaction_.reset();
  pendingNoChangeUiRestore_.reset();
  suppressEditCancelRestore_ = false;
}

void MainWindow::updateUndoAvailability() {
  if (!undoAction_) return;
  if (assemblyProductActive()) {
    const bool isolateActive =
        viewport_ && viewport_->assemblySession() &&
        viewport_->assemblySession()->isolationActive();
    bool canUndo = assemblyCommandHistory_.canUndo();
    bool canRedo = assemblyCommandHistory_.canRedo();
    if (definitionEditContext_) {
      canUndo = canUndo &&
                assemblyCommandHistory_.currentRevision() !=
                    definitionEditContext_->historyRevisionFloor;
      const auto redoState = assemblyCommandHistory_.nextRedoState();
      canRedo = canRedo && redoState &&
                redoState->get().selectedDefinition ==
                    definitionEditContext_->targetDefinitionId;
    }
    undoAction_->setEnabled(!isolateActive && canUndo);
    if (redoAction_) redoAction_->setEnabled(!isolateActive && canRedo);
    return;
  }
  const bool sketchActive =
      workspaceStack_ && workspaceStack_->currentWidget() == sketchCanvas_;
  undoAction_->setEnabled(sketchActive ? sketchCanvas_->canUndo()
                                       : modelHistory_.canUndo());
  if (redoAction_) {
    redoAction_->setEnabled(sketchActive ? sketchCanvas_->canRedo()
                                         : modelHistory_.canRedo());
  }
  updateUpgradeProjectFormatAction();
  updateCreateComponentFromBodyAvailability();
}

void MainWindow::resetTransientModelingUi() {
  if (!partProjectActive()) return;
  commitReferenceImageEdit();
  // PartDesignCoordinator owns modern sessions, while legacy sketch
  // extrusion and the dedicated Revolve panel have separate presentation.
  // Reset all presentation surfaces together before another ribbon command
  // starts; otherwise switching between generations can leave two parameter
  // windows active at once.
  invalidatePreviewUpdates();
  static_cast<void>(partDesignCoordinator_.cancelAll());
  resetTransientModelingPresentation();
}

void MainWindow::resetTransientModelingPresentation() {
  viewport_->resetToolInteraction();
  pendingReferenceImagePath_.clear();
  pendingReferenceImagePixelSize_ = {};
  selectedExtrusionSource_.reset();
  if (toolParametersDock_) toolParametersDock_->hide();
  if (revolveDock_) revolveDock_->hide();
  if (mirrorDock_) mirrorDock_->hide();
  if (moveDock_) moveDock_->hide();
  if (linearPatternDock_) linearPatternDock_->hide();
  if (circularPatternDock_) circularPatternDock_->hide();
  if (extrusionDock_) extrusionDock_->hide();
}

void MainWindow::cancelLegacyExtrusion() {
  if (!partProjectActive()) return;
  // Cancellation is a generation boundary: reject a queued automatic
  // operation detector before clearing any of the state it captured.
  invalidatePreviewUpdates();
  viewport_->resetToolInteraction();
  selectedExtrusionSource_.reset();
  if (extrusionDock_) extrusionDock_->hide();
  if (modelRibbon_) modelRibbon_->clearActiveTool();
}

MainWindow::PreviewSourceSnapshot MainWindow::capturePreviewSource(
    BodyId bodyId, FeatureId featureId) const {
  PreviewSourceSnapshot snapshot;
  snapshot.documentGeneration = previewDocumentGeneration_;
  snapshot.toolRevision = partDesignCoordinator_.revisionToken();
  snapshot.bodyId = bodyId;
  snapshot.featureId = featureId;
  if (bodyId == kInvalidBodyId || featureId == kInvalidFeatureId)
    return snapshot;
  if (const auto* feature = findHistoryFeature(
          static_cast<const Document&>(activePartDocument()), bodyId, featureId)) {
    snapshot.shapeRevision = feature->shapeRevision();
    snapshot.shape = feature->lastValidShape();
  }
  return snapshot;
}

bool MainWindow::previewSourceIsCurrent(
    const PreviewSourceSnapshot& snapshot) const {
  if (snapshot.documentGeneration != previewDocumentGeneration_) return false;
  if (!partDesignCoordinator_.isCurrent(snapshot.toolRevision)) return false;
  if (snapshot.bodyId == kInvalidBodyId ||
      snapshot.featureId == kInvalidFeatureId)
    return !snapshot.shape &&
           snapshot.shapeRevision == kInvalidShapeRevision;
  const auto* feature = findHistoryFeature(
      static_cast<const Document&>(activePartDocument()), snapshot.bodyId,
      snapshot.featureId);
  return feature && feature->shapeRevision() == snapshot.shapeRevision &&
         feature->lastValidShape().get() == snapshot.shape.get();
}

void MainWindow::schedulePreviewUpdate(BodyId bodyId, FeatureId featureId,
                                       std::function<void()> rebuild,
                                       std::function<void()> publish) {
  if (!activePartDesignUiContextAllowed()) return;
  if (!previewUpdates_ || !rebuild || !publish) return;
  const PreviewSourceSnapshot source =
      capturePreviewSource(bodyId, featureId);
  static_cast<void>(previewUpdates_->request(
      [this, source, rebuild = std::move(rebuild),
       publish = std::move(publish)](PreviewUpdateCoordinator::Token token) {
        if (!activePartDesignUiContextAllowed()) return;
        if (!previewUpdates_ || !previewUpdates_->isCurrent(token) ||
            !previewSourceIsCurrent(source))
          return;
        rebuild();
        // OCCT calls are synchronous, but UI callbacks can be re-entered by
        // modal diagnostics or future progress presentation. Revalidate the
        // document/tool generation and immutable source immediately before
        // publishing the result to the viewport.
        if (!previewUpdates_ || !previewUpdates_->isCurrent(token) ||
            !previewSourceIsCurrent(source))
          return;
        if (!previewUpdates_->claimPublication(token)) return;
        publish();
      }));
}

void MainWindow::flushPreviewUpdate() {
  if (!activePartDesignUiContextAllowed()) return;
  if (previewUpdates_) previewUpdates_->flush();
  // Continuous panel/manipulator routes perform at most one builder call.
  // Boundary refinement is reserved for an explicit exact boundary (release,
  // Enter or Apply), and a valid value is a zero-build no-op in the session.
  applyPartDesignUiEffect(partDesignCoordinator_.dispatchActiveInput(
      activePartDocument(), toolParametersPanel_->parameterValue(),
      PartDesignParameterSource::Boundary));
}

void MainWindow::invalidatePreviewUpdates() noexcept {
  if (previewUpdates_) previewUpdates_->invalidate();
  ++previewDocumentGeneration_;
  automaticExtrudeDetectionKey_.reset();
}

ProductDocumentSuccessorStatus
MainWindow::AssemblyProjectionSuccessor::prepare(
    const ProductDocument& candidate,
    ProductDocumentSuccessorImpact impact) noexcept {
  try {
    if (impact == ProductDocumentSuccessorImpact::TreeOnly) {
      return owner_.prepareAssemblyTreeProjection(
          candidate, semanticStateToRestore_.selectedOccurrence,
          semanticStateToRestore_.selectedDefinition,
          treeOnly_, treeSelectionIndex_, preparedSelection_);
    }
    if (impact == ProductDocumentSuccessorImpact::VisibilityProjection) {
      return owner_.prepareAssemblyVisibilityProjection(
          candidate, semanticStateToRestore_.selectedOccurrence,
          visibilityPrepared_,
          treeSelectionIndex_);
    }
    if (impact == ProductDocumentSuccessorImpact::GroundingProjection) {
      return owner_.prepareAssemblyGroundingProjection(
          candidate, semanticStateToRestore_.selectedOccurrence,
          groundingPrepared_,
          treeSelectionIndex_);
    }
    if (impact == ProductDocumentSuccessorImpact::SuppressionProjection) {
      return owner_.prepareAssemblySuppressionProjection(
          candidate, semanticStateToRestore_.selectedOccurrence,
          suppressionPrepared_,
          treeSelectionIndex_);
    }
    if (impact == ProductDocumentSuccessorImpact::DuplicateProjection) {
      return owner_.prepareAssemblyDuplicateProjection(
          candidate, semanticStateToRestore_.selectedOccurrence,
          duplicatePrepared_,
          treeSelectionIndex_);
    }
    if (impact == ProductDocumentSuccessorImpact::RemoveProjection) {
      return owner_.prepareAssemblyRemoveProjection(
          candidate, semanticStateToRestore_.selectedOccurrence,
          removePrepared_,
          treeSelectionIndex_);
    }
    if (impact == ProductDocumentSuccessorImpact::DefinitionProjection) {
      return owner_.prepareAssemblyDefinitionProjection(
          candidate, semanticStateToRestore_.selectedOccurrence,
          semanticStateToRestore_.selectedDefinition,
          definitionPrepared_,
          treeSelectionIndex_);
    }
    return owner_.prepareAssemblyProjection(
        candidate, semanticStateToRestore_, prepared_);
  } catch (const std::bad_alloc&) {
    prepared_.reset();
    visibilityPrepared_.reset();
    groundingPrepared_.reset();
    suppressionPrepared_.reset();
    duplicatePrepared_.reset();
    removePrepared_.reset();
    definitionPrepared_.reset();
    treeOnly_.reset();
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    prepared_.reset();
    visibilityPrepared_.reset();
    groundingPrepared_.reset();
    suppressionPrepared_.reset();
    duplicatePrepared_.reset();
    removePrepared_.reset();
    definitionPrepared_.reset();
    treeOnly_.reset();
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

void MainWindow::undoLastAction() {
  if (assemblyProductActive()) {
    if (assemblyMateActive()) return;
    if (definitionEditContext_ &&
        assemblyCommandHistory_.currentRevision() ==
            definitionEditContext_->historyRevisionFloor)
      return;
    if (viewport_ && viewport_->assemblySession() &&
        viewport_->assemblySession()->isolationActive())
      return;
    cancelAssemblyInsert();
    cancelAssemblyRename();
    AssemblyCommandSemanticState restoreState;
    try {
      const auto restore = assemblyCommandHistory_.nextUndoState();
      if (restore) restoreState = restore->get();
    } catch (...) {
      return;
    }
    const auto linkedDefinition =
        assemblyCommandHistory_.nextUndoLinkedDefinition();
    std::optional<LinkedPartRuntimeState> previousLinkedState;
    if (linkedDefinition) {
      const auto found = linkedPartRuntimeStates_.find(*linkedDefinition);
      if (found != linkedPartRuntimeStates_.end())
        previousLinkedState = found->second;
      linkedPartRuntimeStates_[*linkedDefinition] = {
          LinkedPartRuntimeStatus::Unchecked, {}};
    }
    std::unique_ptr<PreparedAssemblyProjection> successor;
    std::unique_ptr<PreparedAssemblyProjection> visibilitySuccessor;
    std::unique_ptr<PreparedAssemblyProjection> groundingSuccessor;
    std::unique_ptr<PreparedAssemblyProjection> suppressionSuccessor;
    std::unique_ptr<PreparedAssemblyProjection> duplicateSuccessor;
    std::unique_ptr<PreparedAssemblyProjection> removeSuccessor;
    std::unique_ptr<PreparedAssemblyProjection> definitionSuccessor;
    std::unique_ptr<AssemblyTreeModel> treeOnly;
    QModelIndex treeSelectionIndex;
    std::optional<PreparedAssemblySelectionState> preparedSelection;
    AssemblyProjectionSuccessor prepare(
        *this, successor, visibilitySuccessor, groundingSuccessor,
        suppressionSuccessor, duplicateSuccessor, removeSuccessor,
        definitionSuccessor, treeOnly, treeSelectionIndex, preparedSelection,
        std::move(restoreState));
    const auto result =
        assemblyCommandHistory_.undo(productDocument_, prepare);
    if (result.status == AssemblyCommandHistoryStatus::Committed) {
      if (successor)
        installAssemblyProjection(std::move(successor));
      else if (visibilitySuccessor)
        installAssemblyVisibilityProjection(std::move(visibilitySuccessor),
                                            treeSelectionIndex);
      else if (groundingSuccessor)
        installAssemblyGroundingProjection(std::move(groundingSuccessor),
                                           treeSelectionIndex);
      else if (suppressionSuccessor)
        installAssemblySuppressionProjection(std::move(suppressionSuccessor),
                                             treeSelectionIndex);
      else if (duplicateSuccessor)
        installAssemblyDuplicateProjection(std::move(duplicateSuccessor),
                                           treeSelectionIndex);
      else if (removeSuccessor)
        installAssemblyRemoveProjection(std::move(removeSuccessor),
                                        treeSelectionIndex);
      else if (definitionSuccessor)
        installAssemblyDefinitionProjection(std::move(definitionSuccessor),
                                            treeSelectionIndex);
      else
        installAssemblyTreeProjection(
            std::move(treeOnly), treeSelectionIndex,
            std::move(preparedSelection));
      setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
      if (definitionEditContext_) {
        syncSketchPresentationFromDocument();
        refreshBodyViewFromDocument();
        rebuildFeatureTree();
        rebuildHistoryPanel();
        moveHistoryToEnd();
      }
    } else if (linkedDefinition) {
      if (previousLinkedState)
        linkedPartRuntimeStates_[*linkedDefinition] =
            std::move(*previousLinkedState);
      else
        linkedPartRuntimeStates_.erase(*linkedDefinition);
    }
    updateUndoAvailability();
    return;
  }
  if (modelHistory_.nextUndoKind() ==
      std::optional<ModelCommandKind>{ModelCommandKind::ComponentExtraction}) {
    const auto state = modelHistory_.nextUndoComponentWorkspaceState();
    if (!state || componentPresentationEpochClock_ ==
                      std::numeric_limits<std::uint64_t>::max())
      return;
    try {
      const AssemblyMeshProfile profile{
          viewport_->meshQuality() == ViewportMeshQuality::High
              ? AssemblyMeshQuality::High
              : AssemblyMeshQuality::Normal};
      ModelWorkspaceProjectionSuccessor successor(
          ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
          profile, state->get(), componentExtractionProjectionFailurePoint_);
      const auto result =
          modelHistory_.undoComponentExtraction(productDocument_, successor);
      if (result.changed()) {
        componentModelHistories_.clear();
        if (!result.restoredPersistence) std::terminate();
        projectPersistenceState_ = *result.restoredPersistence;
        if (auto part = successor.takePart())
          installPreparedPartProjection(std::move(part));
        else if (auto component = successor.takeComponent())
          installPreparedComponentProjection(std::move(component));
        else
          std::terminate();
        applyProjectKind(EditorProjectKind::Part);
        setWindowModified(!modelHistory_.isAtSavepoint());
      } else {
        statusBar()->showMessage(
            QString::fromUtf8("Отмена создания компонента не применена"),
            4000);
      }
    } catch (...) {
      statusBar()->showMessage(
          QString::fromUtf8("Отмена создания компонента не применена"), 4000);
    }
    updateUndoAvailability();
    return;
  }
  if (modelHistory_.nextUndoKind() ==
      std::optional<ModelCommandKind>{ModelCommandKind::ComponentPlacement}) {
    const auto state = modelHistory_.nextUndoComponentWorkspaceState();
    if (!state || !componentProductActive() || !viewport_ ||
        !viewport_->componentSession() ||
        componentPresentationEpochClock_ ==
            std::numeric_limits<std::uint64_t>::max())
      return;
    try {
      std::optional<ComponentWorkspaceInstanceKey> hovered;
      if (viewport_->componentSession()->hoveredComponent()) {
        const auto& address =
            viewport_->componentSession()->hoveredComponent()->component();
        hovered = ComponentWorkspaceInstanceKey{address.definitionId,
                                                address.occurrencePath};
      }
      const AssemblyMeshProfile profile{
          viewport_->meshQuality() == ViewportMeshQuality::High
              ? AssemblyMeshQuality::High
              : AssemblyMeshQuality::Normal};
      ModelWorkspaceProjectionSuccessor successor(
          ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
          profile, state->get(), componentExtractionProjectionFailurePoint_,
          hovered);
      const auto result =
          modelHistory_.undoComponentPlacement(productDocument_, successor);
      if (result.changed()) {
        if (!result.restoredPersistence) std::terminate();
        projectPersistenceState_ = *result.restoredPersistence;
        auto component = successor.takeComponent();
        if (!component) std::terminate();
        QPointer<MainWindow> receiver(this);
        installPreparedComponentProjection(std::move(component));
        if (!receiver) return;
        receiver->applyProjectKind(EditorProjectKind::Part);
        receiver->setWindowModified(!receiver->modelHistory_.isAtSavepoint());
      } else {
        statusBar()->showMessage(
            QString::fromUtf8("Отмена перемещения компонента не применена"),
            4000);
      }
    } catch (...) {
      statusBar()->showMessage(
          QString::fromUtf8("Отмена перемещения компонента не применена"),
          4000);
    }
    updateUndoAvailability();
    updateCreateComponentFromBodyAvailability();
    return;
  }
  if (modelHistory_.nextUndoKind() ==
      std::optional<ModelCommandKind>{
          ModelCommandKind::ComponentHomePlacement}) {
    const auto state = modelHistory_.nextUndoComponentWorkspaceState();
    if (!state || (!componentProductActive() &&
                   !rootComponentAuthoringActive()) ||
        !viewport_ || !viewport_->componentSession() ||
        componentPresentationEpochClock_ ==
            std::numeric_limits<std::uint64_t>::max())
      return;
    try {
      std::optional<ComponentWorkspaceInstanceKey> hovered;
      if (viewport_->componentSession()->hoveredComponent()) {
        const auto& address =
            viewport_->componentSession()->hoveredComponent()->component();
        hovered = ComponentWorkspaceInstanceKey{address.definitionId,
                                                address.occurrencePath};
      }
      const AssemblyMeshProfile profile{
          viewport_->meshQuality() == ViewportMeshQuality::High
              ? AssemblyMeshQuality::High
              : AssemblyMeshQuality::Normal};
      ModelWorkspaceProjectionSuccessor successor(
          ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
          profile, state->get(), componentExtractionProjectionFailurePoint_,
          hovered);
      const auto result = modelHistory_.undoComponentHomePlacement(
          productDocument_, successor);
      if (result.changed()) {
        if (!result.restoredPersistence) std::terminate();
        projectPersistenceState_ = *result.restoredPersistence;
        if (auto component = successor.takeComponent()) {
          if (rootComponentAuthoringActive())
            installActiveComponentProjection(std::move(component));
          else
            installPreparedComponentProjection(std::move(component));
        }
        applyProjectKind(EditorProjectKind::Part);
        setWindowModified(!modelHistory_.isAtSavepoint());
      } else {
        statusBar()->showMessage(
            QString::fromUtf8(
                "Отмена изменения исходного положения не применена"),
            4000);
      }
    } catch (...) {
      statusBar()->showMessage(
          QString::fromUtf8(
              "Отмена изменения исходного положения не применена"),
          4000);
    }
    updateUndoAvailability();
    updateCreateComponentFromBodyAvailability();
    return;
  }
  resetTransientModelingUi();
  if (workspaceStack_->currentWidget() == sketchCanvas_) {
    if (sketchCanvas_->canUndo()) {
      sketchCanvas_->undo();
      setWindowModified(true);
    }
  } else {
    const auto result =
        modelHistory_.undo(mutableStandaloneDocument(), captureHistorySelection());
    if (result.changed) {
      if (componentDefinitionEditContext_ &&
          !commitActiveComponentDocument()) {
        statusBar()->showMessage(
            QString::fromUtf8("Отмена внутри компонента не применена"), 4000);
        updateUndoAvailability();
        return;
      }
      synchronizeCommittedState(result.state);
    } else if (!result.error.empty()) {
      statusBar()->showMessage(
          QString::fromUtf8("Отмена не применена: %1")
              .arg(QString::fromStdString(result.error)),
          4000);
    }
  }
  updateUndoAvailability();
}

void MainWindow::redoLastAction() {
  if (assemblyProductActive()) {
    if (assemblyMateActive()) return;
    if (definitionEditContext_) {
      const auto next = assemblyCommandHistory_.nextRedoState();
      if (!next || next->get().selectedDefinition !=
                       definitionEditContext_->targetDefinitionId)
        return;
    }
    if (viewport_ && viewport_->assemblySession() &&
        viewport_->assemblySession()->isolationActive())
      return;
    cancelAssemblyInsert();
    cancelAssemblyRename();
    AssemblyCommandSemanticState restoreState;
    try {
      const auto restore = assemblyCommandHistory_.nextRedoState();
      if (restore) restoreState = restore->get();
    } catch (...) {
      return;
    }
    const auto linkedDefinition =
        assemblyCommandHistory_.nextRedoLinkedDefinition();
    std::optional<LinkedPartRuntimeState> previousLinkedState;
    if (linkedDefinition) {
      const auto found = linkedPartRuntimeStates_.find(*linkedDefinition);
      if (found != linkedPartRuntimeStates_.end())
        previousLinkedState = found->second;
      linkedPartRuntimeStates_[*linkedDefinition] = {
          LinkedPartRuntimeStatus::Unchecked, {}};
    }
    std::unique_ptr<PreparedAssemblyProjection> successor;
    std::unique_ptr<PreparedAssemblyProjection> visibilitySuccessor;
    std::unique_ptr<PreparedAssemblyProjection> groundingSuccessor;
    std::unique_ptr<PreparedAssemblyProjection> suppressionSuccessor;
    std::unique_ptr<PreparedAssemblyProjection> duplicateSuccessor;
    std::unique_ptr<PreparedAssemblyProjection> removeSuccessor;
    std::unique_ptr<PreparedAssemblyProjection> definitionSuccessor;
    std::unique_ptr<AssemblyTreeModel> treeOnly;
    QModelIndex treeSelectionIndex;
    std::optional<PreparedAssemblySelectionState> preparedSelection;
    AssemblyProjectionSuccessor prepare(
        *this, successor, visibilitySuccessor, groundingSuccessor,
        suppressionSuccessor, duplicateSuccessor, removeSuccessor,
        definitionSuccessor, treeOnly, treeSelectionIndex, preparedSelection,
        std::move(restoreState));
    const auto result =
        assemblyCommandHistory_.redo(productDocument_, prepare);
    if (result.status == AssemblyCommandHistoryStatus::Committed) {
      if (successor)
        installAssemblyProjection(std::move(successor));
      else if (visibilitySuccessor)
        installAssemblyVisibilityProjection(std::move(visibilitySuccessor),
                                            treeSelectionIndex);
      else if (groundingSuccessor)
        installAssemblyGroundingProjection(std::move(groundingSuccessor),
                                           treeSelectionIndex);
      else if (suppressionSuccessor)
        installAssemblySuppressionProjection(std::move(suppressionSuccessor),
                                             treeSelectionIndex);
      else if (duplicateSuccessor)
        installAssemblyDuplicateProjection(std::move(duplicateSuccessor),
                                           treeSelectionIndex);
      else if (removeSuccessor)
        installAssemblyRemoveProjection(std::move(removeSuccessor),
                                        treeSelectionIndex);
      else if (definitionSuccessor)
        installAssemblyDefinitionProjection(std::move(definitionSuccessor),
                                            treeSelectionIndex);
      else
        installAssemblyTreeProjection(
            std::move(treeOnly), treeSelectionIndex,
            std::move(preparedSelection));
      setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
      if (definitionEditContext_) {
        syncSketchPresentationFromDocument();
        refreshBodyViewFromDocument();
        rebuildFeatureTree();
        rebuildHistoryPanel();
        moveHistoryToEnd();
      }
    } else if (linkedDefinition) {
      if (previousLinkedState)
        linkedPartRuntimeStates_[*linkedDefinition] =
            std::move(*previousLinkedState);
      else
        linkedPartRuntimeStates_.erase(*linkedDefinition);
    }
    updateUndoAvailability();
    return;
  }
  if (modelHistory_.nextRedoKind() ==
      std::optional<ModelCommandKind>{ModelCommandKind::ComponentExtraction}) {
    const auto state = modelHistory_.nextRedoComponentWorkspaceState();
    if (!state || componentPresentationEpochClock_ ==
                      std::numeric_limits<std::uint64_t>::max())
      return;
    try {
      const AssemblyMeshProfile profile{
          viewport_->meshQuality() == ViewportMeshQuality::High
              ? AssemblyMeshQuality::High
              : AssemblyMeshQuality::Normal};
      ModelWorkspaceProjectionSuccessor successor(
          ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
          profile, state->get(), componentExtractionProjectionFailurePoint_);
      const auto result =
          modelHistory_.redoComponentExtraction(productDocument_, successor);
      if (result.changed()) {
        componentModelHistories_.clear();
        if (!result.restoredPersistence) std::terminate();
        projectPersistenceState_ = *result.restoredPersistence;
        if (auto part = successor.takePart())
          installPreparedPartProjection(std::move(part));
        else if (auto component = successor.takeComponent())
          installPreparedComponentProjection(std::move(component));
        else
          std::terminate();
        applyProjectKind(EditorProjectKind::Part);
        setWindowModified(!modelHistory_.isAtSavepoint());
      } else {
        statusBar()->showMessage(
            QString::fromUtf8("Повтор создания компонента не применён"), 4000);
      }
    } catch (...) {
      statusBar()->showMessage(
          QString::fromUtf8("Повтор создания компонента не применён"), 4000);
    }
    updateUndoAvailability();
    return;
  }
  if (modelHistory_.nextRedoKind() ==
      std::optional<ModelCommandKind>{ModelCommandKind::ComponentPlacement}) {
    const auto state = modelHistory_.nextRedoComponentWorkspaceState();
    if (!state || !componentProductActive() || !viewport_ ||
        !viewport_->componentSession() ||
        componentPresentationEpochClock_ ==
            std::numeric_limits<std::uint64_t>::max())
      return;
    try {
      std::optional<ComponentWorkspaceInstanceKey> hovered;
      if (viewport_->componentSession()->hoveredComponent()) {
        const auto& address =
            viewport_->componentSession()->hoveredComponent()->component();
        hovered = ComponentWorkspaceInstanceKey{address.definitionId,
                                                address.occurrencePath};
      }
      const AssemblyMeshProfile profile{
          viewport_->meshQuality() == ViewportMeshQuality::High
              ? AssemblyMeshQuality::High
              : AssemblyMeshQuality::Normal};
      ModelWorkspaceProjectionSuccessor successor(
          ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
          profile, state->get(), componentExtractionProjectionFailurePoint_,
          hovered);
      const auto result =
          modelHistory_.redoComponentPlacement(productDocument_, successor);
      if (result.changed()) {
        if (!result.restoredPersistence) std::terminate();
        projectPersistenceState_ = *result.restoredPersistence;
        auto component = successor.takeComponent();
        if (!component) std::terminate();
        QPointer<MainWindow> receiver(this);
        installPreparedComponentProjection(std::move(component));
        if (!receiver) return;
        receiver->applyProjectKind(EditorProjectKind::Part);
        receiver->setWindowModified(!receiver->modelHistory_.isAtSavepoint());
      } else {
        statusBar()->showMessage(
            QString::fromUtf8("Повтор перемещения компонента не применён"),
            4000);
      }
    } catch (...) {
      statusBar()->showMessage(
          QString::fromUtf8("Повтор перемещения компонента не применён"),
          4000);
    }
    updateUndoAvailability();
    updateCreateComponentFromBodyAvailability();
    return;
  }
  if (modelHistory_.nextRedoKind() ==
      std::optional<ModelCommandKind>{
          ModelCommandKind::ComponentHomePlacement}) {
    const auto state = modelHistory_.nextRedoComponentWorkspaceState();
    if (!state || (!componentProductActive() &&
                   !rootComponentAuthoringActive()) ||
        !viewport_ || !viewport_->componentSession() ||
        componentPresentationEpochClock_ ==
            std::numeric_limits<std::uint64_t>::max())
      return;
    try {
      std::optional<ComponentWorkspaceInstanceKey> hovered;
      if (viewport_->componentSession()->hoveredComponent()) {
        const auto& address =
            viewport_->componentSession()->hoveredComponent()->component();
        hovered = ComponentWorkspaceInstanceKey{address.definitionId,
                                                address.occurrencePath};
      }
      const AssemblyMeshProfile profile{
          viewport_->meshQuality() == ViewportMeshQuality::High
              ? AssemblyMeshQuality::High
              : AssemblyMeshQuality::Normal};
      ModelWorkspaceProjectionSuccessor successor(
          ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
          profile, state->get(), componentExtractionProjectionFailurePoint_,
          hovered);
      const auto result = modelHistory_.redoComponentHomePlacement(
          productDocument_, successor);
      if (result.changed()) {
        if (!result.restoredPersistence) std::terminate();
        projectPersistenceState_ = *result.restoredPersistence;
        if (auto component = successor.takeComponent()) {
          if (rootComponentAuthoringActive())
            installActiveComponentProjection(std::move(component));
          else
            installPreparedComponentProjection(std::move(component));
        }
        applyProjectKind(EditorProjectKind::Part);
        setWindowModified(!modelHistory_.isAtSavepoint());
      } else {
        statusBar()->showMessage(
            QString::fromUtf8(
                "Повтор изменения исходного положения не применён"),
            4000);
      }
    } catch (...) {
      statusBar()->showMessage(
          QString::fromUtf8(
              "Повтор изменения исходного положения не применён"),
          4000);
    }
    updateUndoAvailability();
    updateCreateComponentFromBodyAvailability();
    return;
  }
  resetTransientModelingUi();
  if (workspaceStack_->currentWidget() == sketchCanvas_) {
    if (sketchCanvas_->canRedo()) {
      sketchCanvas_->redo();
      setWindowModified(true);
    }
    updateUndoAvailability();
    return;
  }
  const auto result =
      modelHistory_.redo(mutableStandaloneDocument(), captureHistorySelection());
  if (result.changed) {
    if (componentDefinitionEditContext_ &&
        !commitActiveComponentDocument()) {
      statusBar()->showMessage(
          QString::fromUtf8("Повтор внутри компонента не применён"), 4000);
      updateUndoAvailability();
      return;
    }
    synchronizeCommittedState(result.state);
  } else if (!result.error.empty()) {
    statusBar()->showMessage(
        QString::fromUtf8("Повтор не применён: %1")
            .arg(QString::fromStdString(result.error)),
        4000);
  }
  updateUndoAvailability();
}

MainWindow::ProjectReplacementConfirmation
MainWindow::confirmProjectReplacement(MainWindow& owner) {
  if (!owner.isWindowModified())
    return ProjectReplacementConfirmation::Proceed;

  QPointer<MainWindow> receiver(&owner);
  QPointer<QMessageBox> prompt = new QMessageBox(
      QMessageBox::Warning, QString::fromUtf8("Несохранённые изменения"),
      QString::fromUtf8(
          "Проект был изменён. Сохранить изменения перед продолжением?"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, &owner);
  prompt->setDefaultButton(QMessageBox::Save);
  prompt->button(QMessageBox::Save)->setText(QString::fromUtf8("Сохранить"));
  prompt->button(QMessageBox::Discard)->setText(
      QString::fromUtf8("Не сохранять"));
  prompt->button(QMessageBox::Cancel)->setText(QString::fromUtf8("Отмена"));

  const auto result =
      static_cast<QMessageBox::StandardButton>(prompt->exec());
  if (prompt) delete prompt;
  if (!receiver)
    return ProjectReplacementConfirmation::ReceiverDestroyed;
  if (result == QMessageBox::Cancel)
    return ProjectReplacementConfirmation::Cancelled;
  if (result == QMessageBox::Discard)
    return ProjectReplacementConfirmation::Proceed;
  if (result != QMessageBox::Save)
    return ProjectReplacementConfirmation::Cancelled;
  receiver->saveProject();
  if (!receiver)
    return ProjectReplacementConfirmation::ReceiverDestroyed;
  return receiver->isWindowModified()
             ? ProjectReplacementConfirmation::Cancelled
             : ProjectReplacementConfirmation::Proceed;
}

void MainWindow::closeEvent(QCloseEvent* event) {
  QPointer<MainWindow> receiver(this);
  const auto confirmation = confirmProjectReplacement(*this);
  if (!receiver ||
      confirmation == ProjectReplacementConfirmation::ReceiverDestroyed)
    return;
  if (confirmation != ProjectReplacementConfirmation::Proceed) {
    event->ignore();
    return;
  }
  receiver->resetTransientModelingUi();
  event->accept();
}

void MainWindow::createProject() {
  if (!partProjectActive()) return;
  QPointer<MainWindow> receiver(this);
  QString path = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Создать проект"), QStringLiteral("Новый проект.solidar"),
      QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
  if (!receiver || path.isEmpty()) return;
  if (!path.endsWith(QStringLiteral(".solidar"), Qt::CaseInsensitive))
    path += QStringLiteral(".solidar");
  const auto confirmation = confirmProjectReplacement(*receiver);
  if (!receiver || confirmation != ProjectReplacementConfirmation::Proceed)
    return;
  const auto created = receiver->projectApplicationService_.createProject(path);
  if (!created.succeeded()) {
    QMessageBox::critical(receiver, QString::fromUtf8("Ошибка создания"),
                          created.detail);
    return;
  }
  // Use exactly the same document-transition path as Open.  Maintaining a
  // second hand-written reset sequence here is what allowed controller/session
  // state to outlive the Document that owned its B-Rep references.
  QString loadError;
  if (!receiver->loadProject(path, &loadError)) {
    QMessageBox::critical(receiver, QString::fromUtf8("Ошибка создания"),
                          loadError);
    return;
  }
  receiver->statusBar()->showMessage(QString::fromUtf8("Создан новый проект"),
                                     3000);
}

void MainWindow::openProject() {
  QPointer<MainWindow> receiver(this);
  const QString path = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Открыть проект"), QString(),
      QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
  if (!receiver || path.isEmpty()) return;

  QString error;
  const auto outcome = receiver->runOpenProjectCommand(
      path,
      [](MainWindow& owner) { return confirmProjectReplacement(owner); },
      &error);
  if (!receiver || outcome == OpenProjectCommandOutcome::ReceiverDestroyed)
    return;
  if (outcome == OpenProjectCommandOutcome::Rejected) {
    QMessageBox::critical(receiver, QString::fromUtf8("Ошибка открытия"),
                          error);
    return;
  }
  if (outcome == OpenProjectCommandOutcome::Opened)
    receiver->statusBar()->showMessage(QString::fromUtf8("Проект открыт"),
                                       3000);
}

MainWindow::OpenProjectCommandOutcome MainWindow::runOpenProjectCommand(
    const QString& path,
    const ProjectReplacementConfirmationCallback& confirmReplacement,
    QString* error) {
  // Type-check before a dirty-project confirmation can offer Save/Discard.
  // The carrier is intentionally discarded: a modal confirmation is
  // re-entrant, so a supported part is staged again after the user responds.
  auto preflight = projectApplicationService_.stageOpenProduct(path);
  if (!preflight.succeeded()) {
    if (error) *error = projectOpenErrorMessage(preflight.status);
    return OpenProjectCommandOutcome::Rejected;
  }
  preflight.staged.reset();

  QPointer<MainWindow> receiver(this);
  const auto confirmation = confirmReplacement(*this);
  if (!receiver ||
      confirmation == ProjectReplacementConfirmation::ReceiverDestroyed)
    return OpenProjectCommandOutcome::ReceiverDestroyed;
  if (confirmation != ProjectReplacementConfirmation::Proceed)
    return OpenProjectCommandOutcome::Cancelled;

  // Do not retain the preflight carrier across the modal callback: the file
  // or editor may have changed re-entrantly. loadProject stages and validates
  // the selected path again before its single commit boundary.
  QString loadError;
  if (!receiver->loadProject(path, &loadError)) {
    if (error) *error = loadError;
    return OpenProjectCommandOutcome::Rejected;
  }
  return OpenProjectCommandOutcome::Opened;
}

bool MainWindow::loadProject(const QString& path, QString* error) {
  auto staged = projectApplicationService_.stageOpenProduct(path);
  if (!staged.succeeded()) {
    if (error) *error = projectOpenErrorMessage(staged.status);
    return false;
  }
  if (staged.staged->rootDefinitionKind() == DefinitionKind::Assembly) {
    auto prepared = prepareAssemblyWorkspace(std::move(*staged.staged));
    if (!prepared) {
      if (error)
        *error = QString::fromUtf8(
            "Не удалось подготовить полное представление сборки.");
      return false;
    }
    commitAssemblyReplacement(std::move(prepared), path);
    return true;
  }
  if (staged.staged->rootDefinitionKind() == DefinitionKind::Component) {
    auto prepared = prepareComponentWorkspace(std::move(*staged.staged));
    if (!prepared) {
      if (error)
        *error = QString::fromUtf8(
            "Не удалось подготовить полное представление компонентов.");
      return false;
    }
    commitComponentReplacement(std::move(prepared), path);
    return true;
  }
  if (staged.staged->rootPartDocument() == nullptr) {
    if (error) *error = QString::fromUtf8("Недопустимый корень проекта.");
    return false;
  }
  commitDocumentReplacement(std::move(*staged.staged),
                            DocumentReplacementOrigin::OpenProject, path);
  return true;
}

void MainWindow::saveProject() {
  if (standalonePartProjectActive()) commitReferenceImageEdit();
  else if (componentProductActive()) {
    if (!componentPersistenceTransitionIsQuiescent()) return;
  } else if (projectKind_ != EditorProjectKind::AssemblyReadOnly ||
           !isWritableAssemblyFormat(projectPersistenceState_.format) ||
           definitionEditContext_ || assemblyInsertController_.active() ||
           assemblyMateActive() ||
           assemblyCommandHistory_.isBusy() || !assemblyRenameDialog_.isNull())
    return;
  // The Sketcher edits a working copy. Commit it before serializing so Save
  // never reports success while silently leaving the visible sketch out of
  // the project file.
  if (standalonePartProjectActive() &&
      workspaceStack_->currentWidget() == sketchCanvas_) {
    finishSketch();
    if (workspaceStack_->currentWidget() == sketchCanvas_) return;
  }

  QString path = windowFilePath();
  if (path.isEmpty()) {
    const bool assemblyFirstSave =
        projectKind_ == EditorProjectKind::AssemblyReadOnly;
    const bool componentFirstSave = componentProductActive();
    const ProductIncarnationId expectedIncarnation =
        productDocument_.incarnation();
    const DefinitionId expectedRoot = productDocument_.rootDefinitionId();
    const auto expectedIdentity = productDocument_.identity();
    const auto expectedHistory = assemblyCommandHistory_.currentRevision();
    const auto expectedModelHistory = modelHistory_.currentRevision();
    const std::uint64_t expectedPresentation =
        assemblyFirstSave ? assemblyPresentationEpochClock_
                          : componentPresentationEpochClock_;
    std::vector<std::pair<DefinitionId, DefinitionStateId>> expectedStates;
    if (assemblyFirstSave || componentFirstSave)
      for (const DefinitionId id :
           productDocument_.definitions().definitionIds())
        expectedStates.emplace_back(
            id, productDocument_.definitionStateId(id));
    QPointer<MainWindow> receiver(this);
    path = QFileDialog::getSaveFileName(
        this, QString::fromUtf8("Сохранить проект"), QStringLiteral("Новый проект.solidar"),
        QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
    if (!receiver || path.isEmpty()) return;
    if (assemblyFirstSave || componentFirstSave) {
      bool statesCurrent =
          productDocument_.definitions().definitionIds().size() ==
          expectedStates.size();
      for (const auto& [id, state] : expectedStates)
        statesCurrent = statesCurrent &&
                        productDocument_.definitionStateId(id) == state;
      const bool workspaceCurrent =
          assemblyFirstSave
              ? projectKind_ == EditorProjectKind::AssemblyReadOnly &&
                    isWritableAssemblyFormat(projectPersistenceState_.format)
              : componentProductActive() &&
                    isWritableComponentFormat(
                        projectPersistenceState_.format) &&
                    componentPersistenceTransitionIsQuiescent();
      if (!workspaceCurrent ||
          productDocument_.incarnation() != expectedIncarnation ||
          productDocument_.rootDefinitionId() != expectedRoot ||
          productDocument_.identity() != expectedIdentity ||
          !windowFilePath().isEmpty() ||
          (assemblyFirstSave &&
           assemblyCommandHistory_.currentRevision() != expectedHistory) ||
          (componentFirstSave &&
           modelHistory_.currentRevision() != expectedModelHistory) ||
          (assemblyFirstSave ? assemblyPresentationEpochClock_
                             : componentPresentationEpochClock_) !=
              expectedPresentation ||
          !statesCurrent || definitionEditContext_ ||
          assemblyInsertController_.active() || assemblyMateActive() ||
          assemblyCommandHistory_.isBusy() ||
          !assemblyRenameDialog_.isNull()) {
        statusBar()->showMessage(
            QString::fromUtf8(
                "Состояние проекта изменилось — сохранение отменено."),
            5000);
        return;
      }
    }
    if (!path.endsWith(QStringLiteral(".solidar"), Qt::CaseInsensitive))
      path += QStringLiteral(".solidar");
  }
  QString error;
  if (!saveProjectToPath(path, &error)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка сохранения"), error);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("Проект сохранён"), 3000);
}

bool MainWindow::saveProjectToPath(const QString& path, QString* error) {
  ProjectApplicationStatus saved;
  if (projectKind_ == EditorProjectKind::AssemblyReadOnly) {
    if (!isWritableAssemblyFormat(projectPersistenceState_.format) ||
        definitionEditContext_ || assemblyInsertController_.active() ||
        assemblyMateActive() ||
        assemblyCommandHistory_.isBusy() || !assemblyRenameDialog_.isNull()) {
      if (error)
        *error = QString::fromUtf8(
            "Сборка не готова к сохранению.");
      return false;
    }
    if (productDocument_.identity() && !windowFilePath().isEmpty() &&
        !pathsReferToSameFile(path, windowFilePath())) {
      if (error)
        *error = QString::fromUtf8(
            "Для нового пути сборки используйте «Сохранить как…».");
      return false;
    }
    std::optional<ProductDocumentIdentity> identity;
    if (!productDocument_.identity())
      identity = newProductIdentityForPath(path);
    saved = projectApplicationService_.saveAssembly(
        path, productDocument_, projectPersistenceState_, std::move(identity));
    if (!saved.succeeded()) {
      if (error) *error = saved.detail;
      return false;
    }
    clearComponentDrawingWorkspace();
    setProjectPath(path);
    assemblyCommandHistory_.markSavepoint();
    setWindowModified(false);
    if (error) error->clear();
    return true;
  }
  if (componentProductActive()) {
    if (!componentPersistenceTransitionIsQuiescent()) {
      if (error)
        *error = QString::fromUtf8(
            "Компонентный проект не готов к сохранению.");
      return false;
    }
    if (productDocument_.identity() && !windowFilePath().isEmpty() &&
        !pathsReferToSameFile(path, windowFilePath())) {
      if (error)
        *error = QString::fromUtf8(
            "Для нового пути компонентного проекта используйте «Сохранить как…».");
      return false;
    }
    std::optional<ProductDocumentIdentity> identity;
    if (!productDocument_.identity())
      identity = newProductIdentityForPath(path);
    std::unique_ptr<PreparedComponentProjection> identityProjection;
    if (identity) {
      auto transition = ComponentIdentityTransition::prepareFirstSave(
          productDocument_, std::move(*identity));
      if (transition)
        identityProjection = prepareComponentIdentityProjection(
            std::move(*transition.transition));
      if (!identityProjection) {
        if (error)
          *error = QString::fromUtf8(
              "Не удалось подготовить представление сохраняемого проекта.");
        return false;
      }
    }
    const auto targetPersistence = componentPersistenceForSave(
        productDocument_, projectPersistenceState_);
    saved = identityProjection
        ? projectApplicationService_.saveComponentIdentityTransition(
              path, productDocument_,
              *identityProjection->identityTransition(), targetPersistence)
        : projectApplicationService_.saveComponent(
              path, productDocument_, std::nullopt, targetPersistence);
    if (!saved.succeeded()) {
      if (error) *error = saved.detail;
      return false;
    }
    clearComponentDrawingWorkspace();
    if (identityProjection)
      installComponentIdentityProjection(std::move(identityProjection));
    projectPersistenceState_ = targetPersistence;
    setProjectPath(path);
    modelHistory_.markSavepoint();
    for (auto& [definitionId, history] : componentModelHistories_) {
      static_cast<void>(definitionId);
      history.markSavepoint();
    }
    setWindowModified(false);
    if (error) error->clear();
    return true;
  }
  if (!standalonePartProjectActive()) {
    if (error) *error = QString::fromUtf8("Проект нельзя сохранить.");
    return false;
  }
  switch (projectPersistenceState_.format) {
    case project::ProjectPersistenceFormat::V3:
    case project::ProjectPersistenceFormat::V4:
      saved = projectApplicationService_.save(path, activePartDocument(),
                                              projectPersistenceState_);
      break;
    case project::ProjectPersistenceFormat::V5:
      saved = projectApplicationService_.save(path, productDocument_);
      break;
    case project::ProjectPersistenceFormat::V6:
    case project::ProjectPersistenceFormat::V7:
    case project::ProjectPersistenceFormat::V8:
    case project::ProjectPersistenceFormat::V9:
    case project::ProjectPersistenceFormat::V10:
    case project::ProjectPersistenceFormat::V11:
    case project::ProjectPersistenceFormat::V12:
      saved = {ProjectApplicationErrorCode::SaveFailed,
               QString::fromUtf8(
                   "Формат сборки нельзя сохранить как отдельную деталь.")};
      break;
  }
  if (!saved.succeeded()) {
    if (error) *error = saved.detail;
    return false;
  }
  clearComponentDrawingWorkspace();
  setProjectPath(path);
  modelHistory_.markSavepoint();
  for (auto& [definitionId, history] : componentModelHistories_) {
    static_cast<void>(definitionId);
    history.markSavepoint();
  }
  setWindowModified(false);
  if (error) error->clear();
  return true;
}

ProjectApplicationStatus MainWindow::saveProductCloneToPath(
    const QString& targetPath, const ProductCloneRequest& request) {
  if (projectPersistenceState_.format !=
          project::ProjectPersistenceFormat::V5 ||
      !saveProjectAsIsEligible() ||
      !projectPersistenceTransitionIsQuiescent()) {
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Документ изделия больше не готов к сохранению копии.")};
  }

  const QString normalizedTarget = normalizedSolidarPath(targetPath);
  if (targetPath.isEmpty() || normalizedTarget.isEmpty()) {
    return {ProjectApplicationErrorCode::SaveFailed,
            QString::fromUtf8("Не указан путь копии проекта.")};
  }
  if (pathsReferToSameFile(normalizedTarget, windowFilePath())) {
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Для текущего файла используйте команду «Сохранить».")};
  }

  const auto saved = projectApplicationService_.saveProductAsClone(
      normalizedTarget, productDocument_, request);
  if (!saved.succeeded()) return saved;

  // The service already published the clone and committed its identity into
  // the existing carrier. Preserve every live model and presentation object.
  setProjectPath(normalizedTarget);
  modelHistory_.markSavepoint();
  setWindowModified(false);
  updateUpgradeProjectFormatAction();
  statusBar()->showMessage(QString::fromUtf8("Копия проекта сохранена"),
                           4000);
  return saved;
}

ProjectApplicationStatus MainWindow::runSaveProjectAsCommand(
    const SaveProjectAsTargetChooser& chooseTarget,
    const ProductCloneRequestFactory& makeProductRequest,
    bool* saveAttempted) {
  if (saveAttempted) *saveAttempted = false;
  if (!saveProjectAsIsEligible()) {
    updateUpgradeProjectFormatAction();
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Текущий проект нельзя сохранить под другим именем.")};
  }
  if (!projectPersistenceTransitionIsQuiescent()) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Завершите или отмените текущую операцию перед сохранением копии."),
        5000);
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8("Есть незавершённая операция.")};
  }

  // QFileDialog enters a nested event loop. Snapshot stable values only and
  // retain no references into the product/model graph across the callback.
  const auto expectedState = projectPersistenceState_;
  const auto expectedIdentity = productDocument_.identity();
  const QString expectedPath = windowFilePath();
  const DefinitionId expectedRoot = productDocument_.rootDefinitionId();
  const std::size_t expectedDefinitionCount =
      productDocument_.definitions().size();
  const std::uint64_t expectedDocumentGeneration = previewDocumentGeneration_;
  const std::uint64_t expectedHistoryRevision = modelHistory_.currentRevision();
  const QString suggestedPath = expectedPath.isEmpty()
      ? QString::fromUtf8("Новый проект.solidar")
      : QFileInfo(expectedPath).absolutePath() + QLatin1Char('/') +
            QFileInfo(expectedPath).completeBaseName() +
            QString::fromUtf8("-копия.solidar");

  QPointer<MainWindow> receiver(this);
  std::optional<QString> selectedTarget;
  try {
    selectedTarget = chooseTarget(suggestedPath);
  } catch (const std::exception& exception) {
    if (!receiver)
      return {ProjectApplicationErrorCode::InvalidProject,
              QString::fromUtf8(
                  "Окно проекта было закрыто во время выбора пути.")};
    if (saveAttempted) *saveAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(exception.what())};
  } catch (...) {
    if (!receiver)
      return {ProjectApplicationErrorCode::InvalidProject,
              QString::fromUtf8(
                  "Окно проекта было закрыто во время выбора пути.")};
    if (saveAttempted) *saveAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8("Неизвестная ошибка выбора пути.")};
  }
  if (!receiver)
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Окно проекта было закрыто во время выбора пути.")};
  if (!selectedTarget || selectedTarget->isEmpty()) return {};

  const QString normalizedTarget = normalizedSolidarPath(*selectedTarget);
  if (projectPersistenceState_ != expectedState ||
      productDocument_.identity() != expectedIdentity ||
      windowFilePath() != expectedPath ||
      productDocument_.rootDefinitionId() != expectedRoot ||
      productDocument_.definitions().size() != expectedDefinitionCount ||
      previewDocumentGeneration_ != expectedDocumentGeneration ||
      modelHistory_.currentRevision() != expectedHistoryRevision ||
      !saveProjectAsIsEligible() ||
      !projectPersistenceTransitionIsQuiescent()) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Состояние проекта изменилось — сохранение копии отменено."),
        5000);
    updateUpgradeProjectFormatAction();
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Состояние проекта изменилось во время выбора пути.")};
  }

  if (pathsReferToSameFile(normalizedTarget, expectedPath)) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Для текущего файла используйте команду «Сохранить»."),
        5000);
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8("Выбран текущий файл проекта.")};
  }

  if (expectedState.format != project::ProjectPersistenceFormat::V5) {
    if (saveAttempted) *saveAttempted = true;
    QString error;
    if (!saveProjectToPath(normalizedTarget, &error))
      return {ProjectApplicationErrorCode::SaveFailed, error};
    statusBar()->showMessage(
        QString::fromUtf8("Проект сохранён под новым именем"), 4000);
    return {};
  }

  ProductCloneRequest request;
  try {
    request = makeProductRequest(normalizedTarget);
  } catch (const std::exception& exception) {
    if (!receiver)
      return {ProjectApplicationErrorCode::InvalidProject,
              QString::fromUtf8(
                  "Окно проекта было закрыто во время подготовки копии.")};
    if (saveAttempted) *saveAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(exception.what())};
  } catch (...) {
    if (!receiver)
      return {ProjectApplicationErrorCode::InvalidProject,
              QString::fromUtf8(
                  "Окно проекта было закрыто во время подготовки копии.")};
    if (saveAttempted) *saveAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8("Неизвестная ошибка подготовки копии.")};
  }
  if (!receiver)
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Окно проекта было закрыто во время подготовки копии.")};
  if (saveAttempted) *saveAttempted = true;
  return saveProductCloneToPath(normalizedTarget, request);
}

ProjectApplicationStatus MainWindow::runComponentSaveAsCommand(
    const SaveProjectAsTargetChooser& chooseTarget,
    bool* saveAttempted) {
  if (saveAttempted) *saveAttempted = false;
  if (!componentPersistenceTransitionIsQuiescent())
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Компонентный проект не готов к сохранению копии.")};

  const ProductIncarnationId expectedIncarnation =
      productDocument_.incarnation();
  const DefinitionId expectedRoot = productDocument_.rootDefinitionId();
  const project::ProjectPersistenceState expectedPersistence =
      projectPersistenceState_;
  const auto expectedIdentity = productDocument_.identity();
  const QString expectedPath = windowFilePath();
  const auto expectedHistory = modelHistory_.currentRevision();
  const bool expectedAtSavepoint = modelHistory_.isAtSavepoint();
  const bool expectedDirty = isWindowModified();
  const std::uint64_t expectedPresentation =
      componentPresentationEpochClock_;
  const ComponentViewportSession* expectedSession =
      viewport_->componentSession();
  const ComponentScenePublicationHandle expectedPublication =
      expectedSession->publicationHandle();
  std::optional<ComponentInstanceAddress> expectedSelection;
  std::optional<ComponentInstanceAddress> expectedHover;
  std::vector<std::pair<DefinitionId, DefinitionStateId>> expectedStates;
  try {
    if (expectedSession->selectedComponent())
      expectedSelection = expectedSession->selectedComponent()->component();
    if (expectedSession->hoveredComponent())
      expectedHover = expectedSession->hoveredComponent()->component();
    for (const DefinitionId id :
         productDocument_.definitions().definitionIds())
      expectedStates.emplace_back(
          id, productDocument_.definitionStateId(id));
  } catch (...) {
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(
                "Не удалось подготовить состояние компонентного проекта.")};
  }

  const QString suggestedPath =
      expectedPath.isEmpty()
          ? QString::fromUtf8("Новый компонентный проект.solidar")
          : QFileInfo(expectedPath).absolutePath() + QLatin1Char('/') +
                QFileInfo(expectedPath).completeBaseName() +
                QString::fromUtf8("-копия.solidar");
  QPointer<MainWindow> receiver(this);
  std::optional<QString> selected;
  componentSaveAsChooserActive_ = true;
  updateUpgradeProjectFormatAction();
  try {
    selected = chooseTarget(suggestedPath);
  } catch (const std::exception& exception) {
    if (receiver) {
      receiver->componentSaveAsChooserActive_ = false;
      receiver->updateUpgradeProjectFormatAction();
    }
    if (saveAttempted) *saveAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(exception.what())};
  } catch (...) {
    if (receiver) {
      receiver->componentSaveAsChooserActive_ = false;
      receiver->updateUpgradeProjectFormatAction();
    }
    if (saveAttempted) *saveAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(
                "Неизвестная ошибка выбора пути компонентного проекта.")};
  }
  if (!receiver)
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Окно проекта было закрыто во время выбора пути.")};
  componentSaveAsChooserActive_ = false;
  updateUpgradeProjectFormatAction();
  if (!selected || selected->isEmpty()) return {};

  const ComponentViewportSession* currentSession =
      viewport_ ? viewport_->componentSession() : nullptr;
  std::optional<ComponentInstanceAddress> currentSelection;
  std::optional<ComponentInstanceAddress> currentHover;
  try {
    if (currentSession && currentSession->selectedComponent())
      currentSelection = currentSession->selectedComponent()->component();
    if (currentSession && currentSession->hoveredComponent())
      currentHover = currentSession->hoveredComponent()->component();
  } catch (...) {
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(
                "Не удалось перепроверить состояние компонентного проекта.")};
  }
  bool statesCurrent =
      productDocument_.definitions().definitionIds().size() ==
      expectedStates.size();
  for (const auto& [id, state] : expectedStates)
    statesCurrent = statesCurrent &&
                    productDocument_.definitionStateId(id) == state;
  if (!componentPersistenceTransitionIsQuiescent() ||
      productDocument_.incarnation() != expectedIncarnation ||
      productDocument_.rootDefinitionId() != expectedRoot ||
      projectPersistenceState_ != expectedPersistence ||
      productDocument_.identity() != expectedIdentity ||
      windowFilePath() != expectedPath ||
      modelHistory_.currentRevision() != expectedHistory ||
      modelHistory_.isAtSavepoint() != expectedAtSavepoint ||
      isWindowModified() != expectedDirty ||
      componentPresentationEpochClock_ != expectedPresentation ||
      currentSession != expectedSession || !currentSession ||
      !expectedPublication.isSameCurrentPublication(
          currentSession->publicationHandle()) ||
      currentSelection != expectedSelection || currentHover != expectedHover ||
      !statesCurrent) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Состояние компонентного проекта изменилось — сохранение отменено."),
        5000);
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Состояние проекта изменилось во время выбора пути.")};
  }

  const QString target = normalizedSolidarPath(*selected);
  if (target.isEmpty() || pathsReferToSameFile(target, expectedPath))
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8("Выбран текущий файл проекта.")};
  if (saveAttempted) *saveAttempted = true;
  ProjectApplicationStatus result;
  if (!expectedIdentity || expectedPath.isEmpty()) {
    QString error;
    if (!saveProjectToPath(target, &error))
      result = {ProjectApplicationErrorCode::SaveFailed, std::move(error)};
  } else {
    const ProductDocumentIdentity replacementIdentity =
        newProductIdentityForPath(target);
    auto transition = ComponentIdentityTransition::prepareSaveAs(
        productDocument_, replacementIdentity);
    auto identityProjection = transition
        ? prepareComponentIdentityProjection(
              std::move(*transition.transition))
        : std::unique_ptr<PreparedComponentProjection>{};
    if (!identityProjection)
      return {ProjectApplicationErrorCode::SaveFailed,
              QString::fromUtf8(
                  "Не удалось подготовить представление сохраняемой копии.")};
    result = projectApplicationService_.saveComponentAsIdentityTransition(
        target, productDocument_, expectedPath,
        *identityProjection->identityTransition(),
        componentPersistenceForSave(productDocument_, expectedPersistence));
    if (result.succeeded()) {
      clearComponentDrawingWorkspace();
      installComponentIdentityProjection(std::move(identityProjection));
      projectPersistenceState_ =
          componentPersistenceForSave(productDocument_, expectedPersistence);
      setProjectPath(target);
      modelHistory_.markSavepoint();
      for (auto& [definitionId, history] : componentModelHistories_) {
        static_cast<void>(definitionId);
        history.markSavepoint();
      }
      setWindowModified(false);
    }
  }
  return result;
}

ProjectApplicationStatus MainWindow::runAssemblySaveAsCommand(
    const SaveProjectAsTargetChooser& chooseTarget,
    bool* saveAttempted) {
  if (saveAttempted) *saveAttempted = false;
  if (projectKind_ != EditorProjectKind::AssemblyReadOnly ||
      !isWritableAssemblyFormat(projectPersistenceState_.format) ||
      definitionEditContext_ || assemblyInsertController_.active() ||
      assemblyMateActive() ||
      assemblyCommandHistory_.isBusy() || !assemblyRenameDialog_.isNull() ||
      assemblySaveAsChooserActive_ || linkedPartRepairChooserActive_ ||
      !viewport_ || !viewport_->assemblySession() ||
      viewport_->assemblySession()->isolationActive())
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8("Сборка не готова к сохранению копии.")};

  const ProductIncarnationId expectedIncarnation =
      productDocument_.incarnation();
  const DefinitionId expectedRoot = productDocument_.rootDefinitionId();
  const project::ProjectPersistenceState expectedPersistence =
      projectPersistenceState_;
  const auto expectedIdentity = productDocument_.identity();
  const QString expectedPath = windowFilePath();
  const auto expectedHistory = assemblyCommandHistory_.currentRevision();
  const auto expectedSavepoint = assemblyCommandHistory_.savepointRevision();
  const bool expectedAtSavepoint = assemblyCommandHistory_.isAtSavepoint();
  const auto expectedPersistenceSnapshot =
      captureAssemblyPersistenceSnapshot(productDocument_,
                                         assemblyCommandHistory_);
  if (!expectedPersistenceSnapshot)
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(
                "Не удалось подготовить снимок состояния сборки.")};
  const std::uint64_t expectedPresentation =
      assemblyPresentationEpochClock_;
  const auto expectedSelection =
      captureAssemblyCommandSemanticState();
  if (!expectedSelection)
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8("Не удалось подготовить состояние выбора.")};
  const AssemblyViewportSession* expectedSession =
      viewport_ ? viewport_->assemblySession() : nullptr;
  if (!expectedSession)
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8("Представление сборки недоступно.")};
  const AssemblyScenePublicationHandle expectedPublication =
      expectedSession->publicationHandle();
  std::optional<OccurrencePath> expectedHover;
  std::optional<OccurrencePath> expectedIsolation;
  try {
    if (expectedSession->hoveredOccurrence())
      expectedHover = expectedSession->hoveredOccurrence()->occurrence.path;
    if (expectedSession->isolatedOccurrence())
      expectedIsolation = expectedSession->isolatedOccurrence()->path;
  } catch (...) {
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(
                "Не удалось подготовить состояние представления.")};
  }
  const QString suggestedPath =
      expectedPath.isEmpty() ? QString::fromUtf8("Новая сборка.solidar")
                             : expectedPath;
  QPointer<MainWindow> receiver(this);
  std::optional<QString> selected;
  assemblySaveAsChooserActive_ = true;
  updateLinkedPartLifecycleAvailability();
  try {
    selected = chooseTarget(suggestedPath);
  } catch (const std::exception& exception) {
    if (receiver) {
      receiver->assemblySaveAsChooserActive_ = false;
      receiver->updateLinkedPartLifecycleAvailability();
    }
    if (saveAttempted) *saveAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(exception.what())};
  } catch (...) {
    if (receiver) {
      receiver->assemblySaveAsChooserActive_ = false;
      receiver->updateLinkedPartLifecycleAvailability();
    }
    if (saveAttempted) *saveAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8("Неизвестная ошибка выбора пути сборки.")};
  }
  if (!receiver)
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Окно сборки было закрыто во время выбора пути.")};
  assemblySaveAsChooserActive_ = false;
  updateLinkedPartLifecycleAvailability();
  if (!selected || selected->isEmpty()) return {};

  const auto currentPersistenceSnapshot =
      captureAssemblyPersistenceSnapshot(productDocument_,
                                         assemblyCommandHistory_);
  const AssemblyViewportSession* currentSession =
      viewport_ ? viewport_->assemblySession() : nullptr;
  if (currentSession != expectedSession)
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Представление сборки изменилось во время выбора пути.")};
  std::optional<OccurrencePath> currentHover;
  std::optional<OccurrencePath> currentIsolation;
  try {
    if (currentSession->hoveredOccurrence())
      currentHover = currentSession->hoveredOccurrence()->occurrence.path;
    if (currentSession->isolatedOccurrence())
      currentIsolation = currentSession->isolatedOccurrence()->path;
  } catch (...) {
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(
                "Не удалось перепроверить состояние представления.")};
  }
  if (projectKind_ != EditorProjectKind::AssemblyReadOnly ||
      !isWritableAssemblyFormat(projectPersistenceState_.format) ||
      projectPersistenceState_ != expectedPersistence ||
      productDocument_.incarnation() != expectedIncarnation ||
      productDocument_.rootDefinitionId() != expectedRoot ||
      productDocument_.identity() != expectedIdentity ||
      windowFilePath() != expectedPath ||
      assemblyCommandHistory_.currentRevision() != expectedHistory ||
      assemblyCommandHistory_.savepointRevision() != expectedSavepoint ||
      assemblyCommandHistory_.isAtSavepoint() != expectedAtSavepoint ||
      assemblyPresentationEpochClock_ != expectedPresentation ||
      captureAssemblyCommandSemanticState() != expectedSelection ||
      !expectedPublication.isSameCurrentPublication(
          currentSession->publicationHandle()) ||
      currentHover != expectedHover || currentIsolation != expectedIsolation ||
      !currentPersistenceSnapshot ||
      currentPersistenceSnapshot->partStates !=
          expectedPersistenceSnapshot->partStates ||
      currentPersistenceSnapshot->assemblies !=
          expectedPersistenceSnapshot->assemblies ||
      currentPersistenceSnapshot->historySize !=
          expectedPersistenceSnapshot->historySize ||
      currentPersistenceSnapshot->historyCursor !=
          expectedPersistenceSnapshot->historyCursor ||
      currentPersistenceSnapshot->retainedBytes !=
          expectedPersistenceSnapshot->retainedBytes ||
      currentPersistenceSnapshot->canRedo !=
          expectedPersistenceSnapshot->canRedo ||
      definitionEditContext_ ||
      assemblyInsertController_.active() || assemblyMateActive() ||
      assemblyCommandHistory_.isBusy() ||
      !assemblyRenameDialog_.isNull() || assemblySaveAsChooserActive_ ||
      linkedPartRepairChooserActive_ || currentSession->isolationActive()) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Состояние сборки изменилось — сохранение отменено."),
        5000);
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Состояние сборки изменилось во время выбора пути.")};
  }

  const QString target = normalizedSolidarPath(*selected);
  if (saveAttempted) *saveAttempted = true;
  ProjectApplicationStatus result;
  if (!expectedIdentity || expectedPath.isEmpty()) {
    QString error;
    if (!saveProjectToPath(target, &error))
      result = {ProjectApplicationErrorCode::SaveFailed, std::move(error)};
  } else {
    result = projectApplicationService_.saveAssemblyAs(
        target, productDocument_, projectPersistenceState_,
        {newProductIdentityForPath(target), expectedPath});
    if (result.succeeded()) {
      setProjectPath(target);
      assemblyCommandHistory_.markSavepoint();
      setWindowModified(false);
    }
  }
  return result;
}

void MainWindow::saveProjectAs() {
  if (componentProductActive()) {
    QPointer<MainWindow> receiver(this);
    const SaveProjectAsTargetChooser chooseTarget =
        [this](const QString& suggestedPath) -> std::optional<QString> {
      const QString selected = QFileDialog::getSaveFileName(
          this, QString::fromUtf8("Сохранить компонентный проект как"),
          suggestedPath,
          QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
      return selected.isEmpty() ? std::nullopt
                                : std::optional<QString>{selected};
    };
    bool saveAttempted = false;
    const auto result =
        runComponentSaveAsCommand(chooseTarget, &saveAttempted);
    if (!receiver) return;
    if (!result.succeeded())
      QMessageBox::critical(this, QString::fromUtf8("Ошибка сохранения копии"),
                            result.detail);
    else if (saveAttempted)
      statusBar()->showMessage(
          QString::fromUtf8("Компонентный проект сохранён"), 3000);
    return;
  }
  if (projectKind_ == EditorProjectKind::AssemblyReadOnly) {
    QPointer<MainWindow> receiver(this);
    const SaveProjectAsTargetChooser chooseTarget =
        [this](const QString& suggestedPath) -> std::optional<QString> {
      const QString selected = QFileDialog::getSaveFileName(
          this, QString::fromUtf8("Сохранить сборку как"), suggestedPath,
          QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
      return selected.isEmpty() ? std::nullopt
                                : std::optional<QString>{selected};
    };
    bool saveAttempted = false;
    const auto result =
        runAssemblySaveAsCommand(chooseTarget, &saveAttempted);
    if (!receiver) return;
    if (!result.succeeded())
      QMessageBox::critical(this, QString::fromUtf8("Ошибка сохранения копии"),
                            result.detail);
    else if (saveAttempted)
      statusBar()->showMessage(QString::fromUtf8("Сборка сохранена"), 3000);
    return;
  }
  if (!standalonePartProjectActive()) return;
  QPointer<MainWindow> receiver(this);
  const SaveProjectAsTargetChooser chooseTarget =
      [this](const QString& suggestedPath) -> std::optional<QString> {
    const QString selected = QFileDialog::getSaveFileName(
        this, QString::fromUtf8("Сохранить проект как"), suggestedPath,
        QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
    if (selected.isEmpty()) return std::nullopt;
    return selected;
  };
  const ProductCloneRequestFactory makeRequest =
      [](const QString& targetPath) {
    const QByteArray uuid = QUuid::createUuid().toRfc4122();
    ProjectId::Bytes bytes{};
    for (qsizetype index = 0;
         index < uuid.size() && index < static_cast<qsizetype>(bytes.size());
         ++index)
      bytes[static_cast<std::size_t>(index)] =
          static_cast<std::uint8_t>(uuid.at(index));
    return ProductCloneRequest{
        ProjectId{bytes}, QFileInfo(targetPath).completeBaseName(),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate)};
  };

  bool saveAttempted = false;
  const auto result =
      runSaveProjectAsCommand(chooseTarget, makeRequest, &saveAttempted);
  if (!receiver) return;
  if (saveAttempted && !result.succeeded()) {
    QString message = QString::fromUtf8("Не удалось сохранить копию проекта.");
    if (!result.detail.isEmpty())
      message += QString::fromUtf8("\n\nТехнические сведения: ") +
                 result.detail;
    QMessageBox::critical(this, QString::fromUtf8("Ошибка сохранения копии"),
                          message);
  }
}

bool MainWindow::projectPersistenceTransitionIsQuiescent() const noexcept {
  if (!standalonePartProjectActive()) return false;
  const bool sketchWorkingCopy =
      workspaceStack_ && workspaceStack_->currentWidget() == sketchCanvas_;
  const bool legacyPartDesignActive =
      selectedExtrusionSource_.has_value() ||
      (extrusionDock_ && extrusionDock_->isVisible());
  const bool referenceImageTransaction =
      !pendingReferenceImagePath_.isEmpty() ||
      pendingReferenceImagePixelSize_.isValid() ||
      referenceImageEditBefore_.has_value() ||
      referenceImageEditSelectionBefore_.has_value();
  return partDesignCoordinator_.activeTool() == PartDesignToolKind::None &&
         !partDesignCoordinator_.commitPending() &&
         !partDesignCoordinator_.isReselecting() && !sketchWorkingCopy &&
         !editUiTransaction_ && !pendingNoChangeUiRestore_ &&
         !legacyPartDesignActive && !referenceImageTransaction;
}

bool MainWindow::componentPersistenceTransitionIsQuiescent() const noexcept {
  if (!componentProductActive() ||
      !isWritableComponentFormat(projectPersistenceState_.format) ||
      componentSaveAsChooserActive_ || !componentTreeModel_ || !viewport_ ||
      !viewport_->componentSession() ||
      !viewport_->componentSession()->scene() ||
      !viewport_->componentSession()->scene()->isCurrent())
    return false;
  return partDesignCoordinator_.activeTool() == PartDesignToolKind::None &&
         !partDesignCoordinator_.commitPending() &&
         !partDesignCoordinator_.isReselecting() && !editUiTransaction_ &&
         !pendingNoChangeUiRestore_ && !assemblyInsertController_.active() &&
         !assemblyMateActive() && !assemblyCommandHistory_.isBusy() &&
         assemblyRenameDialog_.isNull();
}

void MainWindow::updateUpgradeProjectFormatAction() noexcept {
  if (!standalonePartProjectActive()) {
    const bool writableAssembly =
        projectKind_ == EditorProjectKind::AssemblyReadOnly &&
        isWritableAssemblyFormat(projectPersistenceState_.format) &&
        !definitionEditContext_ && !assemblyInsertController_.active() &&
        !assemblyMateActive() &&
        !assemblyCommandHistory_.isBusy() && assemblyRenameDialog_.isNull();
    const bool writableComponent =
        componentProductActive() &&
        isWritableComponentFormat(projectPersistenceState_.format) &&
        componentPersistenceTransitionIsQuiescent();
    if (saveAction_)
      saveAction_->setEnabled(writableAssembly || writableComponent);
    if (saveProjectAsAction_)
      saveProjectAsAction_->setEnabled(writableAssembly || writableComponent);
    if (upgradeProjectFormatAction_)
      upgradeProjectFormatAction_->setEnabled(false);
    if (convertToProductDocumentAction_)
      convertToProductDocumentAction_->setEnabled(false);
    if (relocateProductDocumentAction_)
      relocateProductDocumentAction_->setEnabled(false);
    return;
  }
  if (saveProjectAsAction_)
    saveProjectAsAction_->setEnabled(saveProjectAsIsEligible());
  if (upgradeProjectFormatAction_)
    upgradeProjectFormatAction_->setEnabled(
        projectPersistenceState_.format ==
            project::ProjectPersistenceFormat::V3 &&
        !windowFilePath().isEmpty());
  if (convertToProductDocumentAction_)
    convertToProductDocumentAction_->setEnabled(
        standaloneProductConversionIsEligible());
  if (relocateProductDocumentAction_)
    relocateProductDocumentAction_->setEnabled(productRelocationIsEligible());
}

bool MainWindow::saveProjectAsIsEligible() const noexcept {
  if (componentProductActive())
    return isWritableComponentFormat(projectPersistenceState_.format) &&
           productDocument_.rootDefinitionId().isValid() &&
           productDocument_.definitions().contains(
               productDocument_.rootDefinitionId()) &&
           (!productDocument_.identity() ||
            productDocument_.identity()->projectId.isValid());
  if (!standalonePartProjectActive()) return false;
  const auto format = projectPersistenceState_.format;
  const auto& identity = productDocument_.identity();
  const bool singleValidRoot =
      productDocument_.hasSingleValidRootDefinition();
  const bool standalone =
      (format == project::ProjectPersistenceFormat::V3 ||
       format == project::ProjectPersistenceFormat::V4) &&
      !identity.has_value();
  const bool product = format == project::ProjectPersistenceFormat::V5 &&
                       identity.has_value() && identity->projectId.isValid();
  return singleValidRoot && (standalone || product);
}

bool MainWindow::standaloneProductConversionIsEligible() const noexcept {
  if (!standalonePartProjectActive()) return false;
  const auto format = projectPersistenceState_.format;
  const DefinitionId root = productDocument_.rootDefinitionId();
  return (format == project::ProjectPersistenceFormat::V3 ||
          format == project::ProjectPersistenceFormat::V4) &&
         !productDocument_.identity().has_value() &&
         !historicalLegacyExtrusionSourceSketchId_.has_value() &&
         root.isValid() &&
         productDocument_.definitions().size() == 1 &&
         productDocument_.definitions().contains(root);
}

bool MainWindow::productRelocationIsEligible() const noexcept {
  if (!standalonePartProjectActive()) return false;
  const auto& identity = productDocument_.identity();
  return project::productEnvelopeV5RelocationIsSupported() &&
         projectPersistenceState_.format ==
             project::ProjectPersistenceFormat::V5 &&
         identity.has_value() && identity->projectId.isValid() &&
         productDocument_.hasSingleValidRootDefinition() &&
         !windowFilePath().isEmpty() && !isWindowModified() &&
         modelHistory_.isAtSavepoint() &&
         !historicalLegacyExtrusionSourceSketchId_.has_value() &&
         projectPersistenceTransitionIsQuiescent();
}

void MainWindow::upgradeProjectFormat() {
  if (!partProjectActive()) return;
  if (projectPersistenceState_.format ==
          project::ProjectPersistenceFormat::V4 ||
      projectPersistenceState_.format ==
          project::ProjectPersistenceFormat::V5 ||
      projectPersistenceState_.format ==
          project::ProjectPersistenceFormat::V6 ||
      projectPersistenceState_.format ==
          project::ProjectPersistenceFormat::V7 ||
      projectPersistenceState_.format ==
          project::ProjectPersistenceFormat::V8 ||
      projectPersistenceState_.format ==
          project::ProjectPersistenceFormat::V9) {
    updateUpgradeProjectFormatAction();
    return;
  }
  if (windowFilePath().isEmpty()) {
    updateUpgradeProjectFormatAction();
    return;
  }
  if (!projectPersistenceTransitionIsQuiescent()) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Завершите или отмените текущую операцию перед обновлением формата."),
        5000);
    return;
  }

  const QString expectedPath = windowFilePath();
  const auto expectedState = projectPersistenceState_;
  const std::uint64_t expectedDocumentGeneration = previewDocumentGeneration_;
  const std::uint64_t expectedHistoryRevision = modelHistory_.currentRevision();
  const auto answer = QMessageBox::warning(
      this, QString::fromUtf8("Обновление формата проекта"),
      QString::fromUtf8(
          "Проект будет сохранён в формате v4. Старые версии СОЛИДАР3D могут "
          "не открыть этот файл. Продолжить?"),
      QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
  if (answer != QMessageBox::Yes) return;

  // Modal dialogs are re-entrant. Revalidate only stable values; no pointer
  // captured before exec() is dereferenced here.
  if (projectPersistenceState_ != expectedState ||
      projectPersistenceState_.format !=
          project::ProjectPersistenceFormat::V3 ||
      windowFilePath() != expectedPath ||
      previewDocumentGeneration_ != expectedDocumentGeneration ||
      modelHistory_.currentRevision() != expectedHistoryRevision ||
      !projectPersistenceTransitionIsQuiescent()) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Состояние проекта изменилось — обновление формата отменено."),
        5000);
    updateUpgradeProjectFormatAction();
    return;
  }

  const auto upgraded = projectApplicationService_.upgradeProjectFormat(
      expectedPath, activePartDocument(), expectedState);
  if (!upgraded.succeeded()) {
    QMessageBox::critical(this,
                          QString::fromUtf8("Ошибка обновления формата"),
                          upgraded.detail);
    return;
  }

  projectPersistenceState_.format = project::ProjectPersistenceFormat::V4;
  modelHistory_.markSavepoint();
  setWindowModified(false);
  updateUpgradeProjectFormatAction();
  statusBar()->showMessage(
      QString::fromUtf8("Формат проекта обновлён до v4"), 4000);
}

ProjectApplicationStatus MainWindow::convertProjectToPath(
    const QString& targetPath,
    const StandaloneProductConversionRequest& request) {
  if (!standaloneProductConversionIsEligible() ||
      !projectPersistenceTransitionIsQuiescent()) {
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Проект больше не готов к преобразованию в документ изделия.")};
  }

  QString normalizedTarget = targetPath;
  if (normalizedTarget.isEmpty()) {
    return {ProjectApplicationErrorCode::SaveFailed,
            QString::fromUtf8("Не указан путь документа изделия.")};
  }
  if (!normalizedTarget.endsWith(QStringLiteral(".solidar"),
                                 Qt::CaseInsensitive))
    normalizedTarget += QStringLiteral(".solidar");

  const auto converted = projectApplicationService_.convertStandaloneToProduct(
      normalizedTarget, productDocument_, request,
      {historicalLegacyExtrusionSourceSketchId_});
  if (!converted.succeeded()) return converted;

  // The service has already committed identity into the existing carrier and
  // retained the live root allocation. This is deliberately not a document
  // replacement boundary: history, selection, presentations and previews
  // continue to refer to the same model objects.
  projectPersistenceState_.format = project::ProjectPersistenceFormat::V5;
  setProjectPath(normalizedTarget);
  modelHistory_.markSavepoint();
  setWindowModified(false);
  updateUpgradeProjectFormatAction();
  statusBar()->showMessage(
      QString::fromUtf8("Проект преобразован в документ изделия"), 4000);
  return converted;
}

ProjectApplicationStatus MainWindow::runProductConversionCommand(
    const ProductConversionTargetChooser& chooseTarget,
    const ProductConversionRequestFactory& makeRequest,
    bool* conversionAttempted) {
  if (conversionAttempted) *conversionAttempted = false;
  if (!standaloneProductConversionIsEligible()) {
    updateUpgradeProjectFormatAction();
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Текущий проект нельзя преобразовать в документ изделия.")};
  }
  if (!projectPersistenceTransitionIsQuiescent()) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Завершите или отмените текущую операцию перед преобразованием."),
        5000);
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8("Есть незавершённая операция.")};
  }

  // QFileDialog is modal and therefore re-entrant. Snapshot only stable
  // values; no model or presentation pointer survives the callback.
  const auto expectedState = projectPersistenceState_;
  const bool expectedIdentityAbsent = !productDocument_.identity().has_value();
  const QString expectedPath = windowFilePath();
  const DefinitionId expectedRoot = productDocument_.rootDefinitionId();
  const std::size_t expectedDefinitionCount =
      productDocument_.definitions().size();
  const std::uint64_t expectedDocumentGeneration = previewDocumentGeneration_;
  const std::uint64_t expectedHistoryRevision = modelHistory_.currentRevision();

  const QString suggestedPath = expectedPath.isEmpty()
      ? QString::fromUtf8("Документ изделия.solidar")
      : QFileInfo(expectedPath).absolutePath() + QLatin1Char('/') +
            QFileInfo(expectedPath).completeBaseName() +
            QString::fromUtf8("-изделие.solidar");

  // The chooser can enter a nested event loop. It may close and delete this
  // window, so the callback boundary must not rely on the member-function
  // receiver still being alive when control returns.
  QPointer<MainWindow> receiver(this);
  std::optional<QString> selectedTarget;
  try {
    selectedTarget = chooseTarget(suggestedPath);
  } catch (const std::exception& exception) {
    if (!receiver)
      return {ProjectApplicationErrorCode::InvalidProject,
              QString::fromUtf8(
                  "Окно проекта было закрыто во время выбора пути.")};
    if (conversionAttempted) *conversionAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(exception.what())};
  } catch (...) {
    if (!receiver)
      return {ProjectApplicationErrorCode::InvalidProject,
              QString::fromUtf8(
                  "Окно проекта было закрыто во время выбора пути.")};
    if (conversionAttempted) *conversionAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8("Неизвестная ошибка выбора пути.")};
  }
  if (!receiver)
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Окно проекта было закрыто во время выбора пути.")};
  if (!selectedTarget || selectedTarget->isEmpty()) return {};

  QString normalizedTarget = *selectedTarget;
  if (!normalizedTarget.endsWith(QStringLiteral(".solidar"),
                                 Qt::CaseInsensitive))
    normalizedTarget += QStringLiteral(".solidar");

  if (projectPersistenceState_ != expectedState ||
      (!productDocument_.identity().has_value()) != expectedIdentityAbsent ||
      windowFilePath() != expectedPath ||
      productDocument_.rootDefinitionId() != expectedRoot ||
      productDocument_.definitions().size() != expectedDefinitionCount ||
      previewDocumentGeneration_ != expectedDocumentGeneration ||
      modelHistory_.currentRevision() != expectedHistoryRevision ||
      !standaloneProductConversionIsEligible() ||
      !projectPersistenceTransitionIsQuiescent()) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Состояние проекта изменилось — преобразование отменено."),
        5000);
    updateUpgradeProjectFormatAction();
    return {ProjectApplicationErrorCode::InvalidProject,
            QString::fromUtf8(
                "Состояние проекта изменилось во время выбора пути.")};
  }

  StandaloneProductConversionRequest request;
  try {
    request = makeRequest(normalizedTarget, expectedRoot);
  } catch (const std::exception& exception) {
    if (conversionAttempted) *conversionAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(exception.what())};
  } catch (...) {
    if (conversionAttempted) *conversionAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8("Неизвестная ошибка подготовки преобразования.")};
  }
  if (conversionAttempted) *conversionAttempted = true;
  return convertProjectToPath(normalizedTarget, request);
}

void MainWindow::convertToProductDocument() {
  if (!partProjectActive()) return;
  QPointer<MainWindow> receiver(this);
  const ProductConversionTargetChooser chooseTarget =
      [this](const QString& suggestedPath) -> std::optional<QString> {
    const QString selected = QFileDialog::getSaveFileName(
        this, QString::fromUtf8("Преобразовать в документ изделия"),
        suggestedPath, QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
    if (selected.isEmpty()) return std::nullopt;
    return selected;
  };
  const ProductConversionRequestFactory makeRequest =
      [](const QString& targetPath, DefinitionId rootDefinitionId) {
    const QByteArray uuid = QUuid::createUuid().toRfc4122();
    ProjectId::Bytes bytes{};
    for (qsizetype index = 0;
         index < uuid.size() && index < static_cast<qsizetype>(bytes.size());
         ++index)
      bytes[static_cast<std::size_t>(index)] =
          static_cast<std::uint8_t>(uuid.at(index));
    return StandaloneProductConversionRequest{
        ProjectId{bytes}, rootDefinitionId,
        QFileInfo(targetPath).completeBaseName(),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate)};
  };

  bool conversionAttempted = false;
  const auto result = runProductConversionCommand(
      chooseTarget, makeRequest, &conversionAttempted);
  if (!receiver) return;
  if (conversionAttempted && !result.succeeded()) {
    QString message = QString::fromUtf8(
        "Не удалось преобразовать проект в документ изделия.");
    if (!result.detail.isEmpty())
      message += QString::fromUtf8("\n\nТехнические сведения: ") +
                 result.detail;
    QMessageBox::critical(this, QString::fromUtf8("Ошибка преобразования"),
                          message);
  }
}

ProjectApplicationStatus MainWindow::runProductRelocationCommand(
    const ProductRelocationTargetChooser& chooseTarget,
    const ProductRelocationExecutor& relocate,
    bool* relocationAttempted) {
  if (relocationAttempted) *relocationAttempted = false;
  if (!productRelocationIsEligible()) {
    updateUpgradeProjectFormatAction();
    return {ProjectApplicationErrorCode::RelocateNotEligible,
            QString::fromUtf8(
                "Переместить можно только сохранённый документ изделия без незавершённых изменений.")};
  }

  // QFileDialog enters a nested event loop. Snapshot only stable values and
  // revalidate every eligibility input before any filesystem operation.
  const auto expectedState = projectPersistenceState_;
  const auto expectedIdentity = productDocument_.identity();
  const QString expectedPath = windowFilePath();
  const DefinitionId expectedRoot = productDocument_.rootDefinitionId();
  const std::size_t expectedDefinitionCount =
      productDocument_.definitions().size();
  const std::uint64_t expectedDocumentGeneration = previewDocumentGeneration_;
  const std::uint64_t expectedHistoryRevision = modelHistory_.currentRevision();
  const bool expectedSavepoint = modelHistory_.isAtSavepoint();
  const auto expectedLegacyMarker =
      historicalLegacyExtrusionSourceSketchId_;
  const QString suggestedPath =
      QFileInfo(expectedPath).absolutePath() + QLatin1Char('/') +
      QFileInfo(expectedPath).completeBaseName() +
      QString::fromUtf8("-перемещено.solidar");

  QPointer<MainWindow> receiver(this);
  std::optional<QString> selectedTarget;
  try {
    selectedTarget = chooseTarget(suggestedPath);
  } catch (const std::exception& exception) {
    if (!receiver)
      return {ProjectApplicationErrorCode::RelocateNotEligible,
              QString::fromUtf8(
                  "Окно проекта было закрыто во время выбора пути.")};
    if (relocationAttempted) *relocationAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(exception.what())};
  } catch (...) {
    if (!receiver)
      return {ProjectApplicationErrorCode::RelocateNotEligible,
              QString::fromUtf8(
                  "Окно проекта было закрыто во время выбора пути.")};
    if (relocationAttempted) *relocationAttempted = true;
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8("Неизвестная ошибка выбора пути.")};
  }
  if (!receiver)
    return {ProjectApplicationErrorCode::RelocateNotEligible,
            QString::fromUtf8(
                "Окно проекта было закрыто во время выбора пути.")};
  if (!selectedTarget || selectedTarget->isEmpty()) return {};

  const QString normalizedTarget = normalizedSolidarPath(*selectedTarget);
  if (projectPersistenceState_ != expectedState ||
      productDocument_.identity() != expectedIdentity ||
      windowFilePath() != expectedPath ||
      productDocument_.rootDefinitionId() != expectedRoot ||
      productDocument_.definitions().size() != expectedDefinitionCount ||
      previewDocumentGeneration_ != expectedDocumentGeneration ||
      modelHistory_.currentRevision() != expectedHistoryRevision ||
      modelHistory_.isAtSavepoint() != expectedSavepoint ||
      historicalLegacyExtrusionSourceSketchId_ != expectedLegacyMarker ||
      isWindowModified() || !productRelocationIsEligible()) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Состояние проекта изменилось — перемещение отменено."),
        5000);
    updateUpgradeProjectFormatAction();
    return {ProjectApplicationErrorCode::RelocateNotEligible,
            QString::fromUtf8(
                "Состояние проекта изменилось во время выбора пути.")};
  }

  if (pathsReferToSameFile(normalizedTarget, expectedPath)) {
    if (relocationAttempted) *relocationAttempted = true;
    return {ProjectApplicationErrorCode::RelocateSameFile,
            QString::fromUtf8("Выбран текущий файл проекта.")};
  }

  if (relocationAttempted) *relocationAttempted = true;
  ProjectApplicationStatus relocated;
  try {
    relocated = relocate(
        expectedPath, normalizedTarget,
        {true, historicalLegacyExtrusionSourceSketchId_});
  } catch (const std::exception& exception) {
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8(exception.what())};
  } catch (...) {
    return {ProjectApplicationErrorCode::UnexpectedFailure,
            QString::fromUtf8("Неизвестная ошибка перемещения проекта.")};
  }
  if (!receiver)
    return {ProjectApplicationErrorCode::RelocateNotEligible,
            QString::fromUtf8(
                "Окно проекта было закрыто во время перемещения.")};
  if (!relocated.succeeded()) return relocated;

  // The service moved the already-saved bytes and never touched the carrier.
  // Relocation commits only the locator; history/savepoint/dirty state stay
  // exactly as they were before the command.
  setProjectPath(normalizedTarget);
  updateUpgradeProjectFormatAction();
  statusBar()->showMessage(
      QString::fromUtf8("Документ изделия перемещён"), 4000);
  return {};
}

void MainWindow::relocateProductDocument() {
  if (!partProjectActive()) return;
  QPointer<MainWindow> receiver(this);
  const ProductRelocationTargetChooser chooseTarget =
      [this](const QString& suggestedPath) -> std::optional<QString> {
    const QString selected = QFileDialog::getSaveFileName(
        this, QString::fromUtf8("Переместить документ изделия"),
        suggestedPath, QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
    if (selected.isEmpty()) return std::nullopt;
    return selected;
  };
  const ProductRelocationExecutor relocate =
      [this](const QString& sourcePath, const QString& targetPath,
             const ProductRelocationPrecondition& precondition) {
    return projectApplicationService_.relocateProduct(
        sourcePath, targetPath, productDocument_, precondition);
  };

  bool relocationAttempted = false;
  const auto result = runProductRelocationCommand(
      chooseTarget, relocate, &relocationAttempted);
  if (!receiver) return;
  if (relocationAttempted && !result.succeeded()) {
    QString message =
        QString::fromUtf8("Не удалось переместить документ изделия.");
    if (!result.detail.isEmpty())
      message += QString::fromUtf8("\n\nТехнические сведения: ") +
                 result.detail;
    QMessageBox::critical(this, QString::fromUtf8("Ошибка перемещения"),
                          message);
  }
}

PartDesignCommitGuard MainWindow::startPartDesignCommit() {
  if (!activePartDesignUiContextAllowed()) return {};
  const auto kind = partDesignCoordinator_.activeTool();
  if (projectPersistenceState_.format ==
          project::ProjectPersistenceFormat::V3 &&
      kind != PartDesignToolKind::None &&
      containsV2TopologyReference(
          partDesignCoordinator_.snapshot(kind, activePartDocument()))) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Для устойчивых ссылок v2 сначала выберите «Файл → Обновить формат проекта до v4…»."),
        6000);
    return {};
  }
  return partDesignCoordinator_.startCommit();
}

void MainWindow::importStep() {
  if (!standalonePartProjectActive()) return;
  const QString fileName = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Импортировать STEP"), QString(),
      QStringLiteral("STEP files (*.step *.stp)"));
  if (fileName.isEmpty()) return;

  const QString importedName = QFileInfo(fileName).completeBaseName();
  auto staged =
      projectPersistenceState_.format == project::ProjectPersistenceFormat::V5
          ? projectApplicationService_.stageImportStep(
                fileName, productDocument_, importedName)
          : projectApplicationService_.stageImportStep(
                fileName, activePartDocument(), importedName);
  if (!staged.succeeded()) {
    QMessageBox::critical(
        this, QString::fromUtf8("Не удалось импортировать STEP"),
        staged.status.detail);
    return;
  }
  commitDocumentReplacement(std::move(*staged.staged),
                            DocumentReplacementOrigin::ImportedStep,
                            fileName);
}

void MainWindow::importReferenceImage() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) {
    modelRibbon_->clearActiveTool();
    return;
  }
  const QString fileName = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Добавить изображение"), QString(),
      QString::fromUtf8(
          "Изображения (*.png *.jpg *.jpeg *.bmp *.webp);;Все файлы (*)"));
  if (fileName.isEmpty()) {
    modelRibbon_->clearActiveTool();
    return;
  }
  QImageReader reader(fileName);
  if (!reader.canRead() || !reader.size().isValid()) {
    QMessageBox::warning(this, QString::fromUtf8("Изображение"),
                         QString::fromUtf8("Не удалось прочитать выбранный файл."));
    modelRibbon_->clearActiveTool();
    return;
  }
  if (reader.size().width() > 16384 || reader.size().height() > 16384 ||
      static_cast<qint64>(reader.size().width()) * reader.size().height() >
          100000000LL) {
    QMessageBox::warning(
        this, QString::fromUtf8("Изображение"),
        QString::fromUtf8("Изображение слишком большое (максимум 100 мегапикселей)."));
    modelRibbon_->clearActiveTool();
    return;
  }
  resetTransientModelingUi();
  pendingReferenceImagePath_ = QFileInfo(fileName).absoluteFilePath();
  pendingReferenceImagePixelSize_ = reader.size();
  statusBar()->showMessage(
      QString::fromUtf8("Выберите базовую плоскость или плоскую грань для изображения"));
  viewport_->beginImagePlaneSelection();
}

void MainWindow::finishReferenceImageImport(const SketchPlanePick& pick) {
  if (!partProjectActive()) return;
  if (pendingReferenceImagePath_.isEmpty() ||
      !pendingReferenceImagePixelSize_.isValid())
    return;
  SketchPlacement placement = SketchPlacement::xy();
  if (const auto* datum = std::get_if<DatumPlanePick>(&pick.source)) {
    placement = datum->placement;
  } else if (const auto* legacy =
                 std::get_if<LegacySolidFacePick>(&pick.source)) {
    placement = legacy->placement;
  } else if (const auto* bodyFace = std::get_if<BodyFacePick>(&pick.source)) {
    const auto& reference = bodyFace->face;
    const ShapeFeature* feature =
        activePartDocument().findFeature(reference.bodyId(), reference.featureId());
    const auto topology = feature ? feature->topologyIndex() : nullptr;
    const auto resolvedFace =
        topology ? topology->resolveFace(reference) : FaceResolution{};
    const auto resolved = resolvedFace
                              ? resolveFacePlacement(*resolvedFace.subshape)
                              : ResolvedFacePlacement{};
    if (!resolved.resolved || !resolved.planar) {
      QMessageBox::warning(
          this, QString::fromUtf8("Изображение"),
          QString::fromUtf8("Изображение можно разместить только на доступной плоской грани."));
      pendingReferenceImagePath_.clear();
      modelRibbon_->clearActiveTool();
      rebuildFeatureTree();
      return;
    }
    placement = resolved.placement;
  }

  const auto previousSelection = captureHistorySelection();
  Document previous = activePartDocument();
  ReferenceImageId addedId = kInvalidReferenceImageId;
  try {
    auto& image = mutableStandaloneDocument().addReferenceImage(
        QFileInfo(pendingReferenceImagePath_).completeBaseName().toStdString(),
        pendingReferenceImagePath_.toStdString(),
        pick.presentationLabel.toStdString(), placement,
        pendingReferenceImagePixelSize_.width(),
        pendingReferenceImagePixelSize_.height());
    addedId = image.id;
  } catch (const std::exception& exception) {
    QMessageBox::critical(this, QString::fromUtf8("Изображение"),
                          QString::fromUtf8("Не удалось добавить изображение: %1")
                              .arg(QString::fromUtf8(exception.what())));
    pendingReferenceImagePath_.clear();
    pendingReferenceImagePixelSize_ = {};
    modelRibbon_->clearActiveTool();
    return;
  }
  if (!pushModelTransition(std::move(previous), previousSelection,
                           captureCommittedEndState())) {
    pendingReferenceImagePath_.clear();
    pendingReferenceImagePixelSize_ = {};
    modelRibbon_->clearActiveTool();
    return;
  }
  pendingReferenceImagePath_.clear();
  pendingReferenceImagePixelSize_ = {};
  selectedReferenceImageId_ = addedId;
  modelRibbon_->clearActiveTool();
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Изображение добавлено"), 3000);
}

void MainWindow::updateReferenceImageParameter(int parameterIndex,
                                               double value) {
  if (!partProjectActive()) return;
  auto* image = mutableStandaloneDocument().findReferenceImage(selectedReferenceImageId_);
  if (!image || !std::isfinite(value) || parameterIndex < 0 ||
      parameterIndex > 3)
    return;
  const double modelValue = parameterIndex == 3 ? value / 100.0 : value;
  const double current = parameterIndex == 0   ? image->offsetXMm
                         : parameterIndex == 1 ? image->offsetYMm
                         : parameterIndex == 2 ? image->offsetZMm
                                               : image->scale;
  if (std::abs(current - modelValue) <= 1e-12) return;

  if (!referenceImageEditBefore_) {
    referenceImageEditBefore_ = activePartDocument();
    referenceImageEditSelectionBefore_ = captureHistorySelection();
  }
  if (parameterIndex == 0)
    image->offsetXMm = modelValue;
  else if (parameterIndex == 1)
    image->offsetYMm = modelValue;
  else if (parameterIndex == 2)
    image->offsetZMm = modelValue;
  else
    image->scale = modelValue;

  // Keep drag and typed edits live without creating one Undo step per mouse
  // move. The snapshot is committed once on release/Enter/selection change.
  setWindowModified(true);
  viewport_->setReferenceImages(activePartDocument().referenceImages());
  viewport_->setReferenceImageManipulator(*image);
}

void MainWindow::commitReferenceImageEdit() {
  if (!partProjectActive()) return;
  if (!referenceImageEditBefore_) return;
  Document previous = std::move(*referenceImageEditBefore_);
  HistorySelectionState previousSelection =
      referenceImageEditSelectionBefore_.value_or(captureHistorySelection());
  referenceImageEditBefore_.reset();
  referenceImageEditSelectionBefore_.reset();
  if (!pushModelTransition(std::move(previous), std::move(previousSelection),
                           captureCommittedEndState())) {
    viewport_->setReferenceImages(activePartDocument().referenceImages());
    showSelectedReferenceImageManipulator();
    return;
  }
  completeModelTransitionUi();
}

void MainWindow::showSelectedReferenceImageManipulator() {
  if (!partProjectActive()) return;
  const auto* image = mutableStandaloneDocument().findReferenceImage(selectedReferenceImageId_);
  if (!image || !image->visible) {
    if (viewport_->referenceImageManipulatorActive())
      viewport_->clearToolManipulator();
    return;
  }
  viewport_->setReferenceImageManipulator(*image);
  statusBar()->showMessage(
      QString::fromUtf8("Потяните стрелку X, Y или Z либо введите смещение и масштаб"));
}

void MainWindow::removeReferenceImage(ReferenceImageId id) {
  if (!partProjectActive()) return;
  commitReferenceImageEdit();
  if (!activePartDocument().findReferenceImage(id)) return;
  const auto previousSelection = captureHistorySelection();
  Document previous = activePartDocument();
  if (!mutableStandaloneDocument().removeReferenceImage(id)) return;
  if (!pushModelTransition(std::move(previous), previousSelection,
                           captureCommittedEndState()))
    return;
  if (selectedReferenceImageId_ == id) {
    selectedReferenceImageId_ = kInvalidReferenceImageId;
    if (viewport_->referenceImageManipulatorActive())
      viewport_->clearToolManipulator();
  }
  viewport_->setReferenceImages(activePartDocument().referenceImages());
  rebuildFeatureTree();
  completeModelTransitionUi();
}

void MainWindow::applyImportedDocument(Document staged,
                                       const QString& importedName) {
  commitDocumentReplacement(StagedApplicationDocument(
                                std::move(staged), std::nullopt, std::nullopt),
                            DocumentReplacementOrigin::ImportedStep,
                            importedName);
}

std::unique_ptr<PreparedAssemblyWorkspace>
MainWindow::prepareAssemblyWorkspace(StagedApplicationDocument staged) noexcept {
  if (assemblyPresentationEpochClock_ ==
      std::numeric_limits<std::uint64_t>::max())
    return {};
  const ProductPresentationEpoch candidateEpoch{
      assemblyPresentationEpochClock_ + 1};
  const AssemblyMeshProfile profile{
      viewport_ && viewport_->meshQuality() == ViewportMeshQuality::High
          ? AssemblyMeshQuality::High
          : AssemblyMeshQuality::Normal};
  auto prepared = AssemblyWorkspacePreparer::prepare(
      std::move(staged), candidateEpoch, profile,
      assemblyPreparationFailurePoint_);
  if (!prepared) return {};
  return std::move(prepared.workspace);
}

std::unique_ptr<PreparedComponentWorkspace>
MainWindow::prepareComponentWorkspace(
    StagedApplicationDocument staged) noexcept {
  if (componentPresentationEpochClock_ ==
      std::numeric_limits<std::uint64_t>::max())
    return {};
  const ProductPresentationEpoch candidateEpoch{
      componentPresentationEpochClock_ + 1};
  const AssemblyMeshProfile profile{
      viewport_ && viewport_->meshQuality() == ViewportMeshQuality::High
          ? AssemblyMeshQuality::High
          : AssemblyMeshQuality::Normal};
  auto prepared = ComponentWorkspacePreparer::prepare(
      std::move(staged), candidateEpoch, profile,
      componentPreparationFailurePoint_, componentTreeFailurePoint_);
  if (!prepared) return {};
  return std::move(prepared.workspace);
}

ProductDocumentSuccessorStatus MainWindow::prepareAssemblyProjection(
    const ProductDocument& product,
    const AssemblyCommandSemanticState& semanticStateToRestore,
    std::unique_ptr<PreparedAssemblyProjection>& projection) noexcept {
  projection.reset();
  try {
    if (assemblyPresentationEpochClock_ ==
        std::numeric_limits<std::uint64_t>::max())
      return ProductDocumentSuccessorStatus::Rejected;
    const ProductPresentationEpoch candidateEpoch{
        assemblyPresentationEpochClock_ + 1};
    const AssemblyMeshProfile profile{
        viewport_ && viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto prepared = AssemblyProjectionPreparer::prepare(
        product, candidateEpoch, profile, assemblyPreparationFailurePoint_,
        &linkedPartRuntimeStates_);
    if (!prepared)
      return prepared.status == AssemblyWorkspacePrepareStatus::ResourceFailed
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    if (!prepared.projection->session || !prepared.projection->treeModel)
      return ProductDocumentSuccessorStatus::Rejected;
    if (assemblyFullProjectionSelectionResourceFailureForTesting_)
      throw std::bad_alloc();

    std::optional<AssemblyOccurrenceSelection> desiredSelection;
    if (semanticStateToRestore.selectedOccurrence) {
      desiredSelection = AssemblyOccurrenceSelection{
          prepared.projection->session->publicationHandle(),
          {candidateEpoch, product.rootDefinitionId(),
           *semanticStateToRestore.selectedOccurrence}};
      prepared.projection->selectionIndex =
          prepared.projection->treeModel->indexForOccurrence(*desiredSelection);
      if (!prepared.projection->selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    auto selected = prepared.projection->session->prepareSelectedOccurrence(
        desiredSelection);
    if (selected.status != PrepareAssemblySelectionStatus::Prepared)
      return selected.status == PrepareAssemblySelectionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    prepared.projection->session->installPreparedSelectedOccurrence(
        std::move(selected.state));

    if (semanticStateToRestore.selectedDefinition) {
      prepared.projection->selectionIndex =
          prepared.projection->treeModel->indexForDefinition(
              *semanticStateToRestore.selectedDefinition);
      if (!prepared.projection->selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    projection = std::move(prepared.projection);
    return ProductDocumentSuccessorStatus::Prepared;
  } catch (const std::bad_alloc&) {
    projection.reset();
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    projection.reset();
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

ProductDocumentSuccessorStatus MainWindow::prepareAssemblyTreeProjection(
    const ProductDocument& product,
    std::optional<OccurrencePath> selectionToRestore,
    std::optional<DefinitionId> definitionSelectionToRestore,
    std::unique_ptr<AssemblyTreeModel>& tree,
    QModelIndex& selectionIndex,
    std::optional<PreparedAssemblySelectionState>& preparedSelection) {
  tree.reset();
  selectionIndex = {};
  preparedSelection.reset();
  if (assemblyPreparationFailurePoint_ !=
      AssemblyViewportSessionFailurePoint::None)
    return ProductDocumentSuccessorStatus::Rejected;
  if (!viewport_ || !viewport_->assemblySession())
    return ProductDocumentSuccessorStatus::Rejected;
  const auto* session = viewport_->assemblySession();
  const auto failurePoint = assemblyTreeResourceFailureForTesting_
                                ? AssemblyTreeModelFailurePoint::ResourceFailure
                                : AssemblyTreeModelFailurePoint::None;
  auto created = AssemblyTreeModel::createTyped(
      product, session->epoch(), session->publicationHandle(), nullptr,
      failurePoint,
      session->isolatedOccurrence()
          ? std::optional<OccurrencePath>{
                session->isolatedOccurrence()->path}
          : std::nullopt,
      &linkedPartRuntimeStates_);
  if (!created)
    return created.status == AssemblyTreeModelCreateStatus::ResourceFailure
               ? ProductDocumentSuccessorStatus::ResourceFailure
               : ProductDocumentSuccessorStatus::Rejected;

  // Rename is tree-only and must retain the exact viewport interaction state.
  // Resolve the replacement tree index while allocation is still allowed;
  // installation after model/history commit performs no path construction.
  const auto& currentSelection = viewport_->selectedAssemblyOccurrenceView();
  std::optional<AssemblyOccurrenceSelection> desiredSelection;
  if (selectionToRestore) {
    desiredSelection = AssemblyOccurrenceSelection{
        session->publicationHandle(),
        {session->epoch(), product.rootDefinitionId(), *selectionToRestore}};
    selectionIndex = created.model->indexForOccurrence(*desiredSelection);
    if (!selectionIndex.isValid())
      return ProductDocumentSuccessorStatus::Rejected;
  }
  if (currentSelection != desiredSelection) {
    auto prepared = session->prepareSelectedOccurrence(desiredSelection);
    if (prepared.status != PrepareAssemblySelectionStatus::Prepared)
      return prepared.status == PrepareAssemblySelectionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    preparedSelection = std::move(prepared.state);
  }
  if (definitionSelectionToRestore) {
    selectionIndex =
        created.model->indexForDefinition(*definitionSelectionToRestore);
    if (!selectionIndex.isValid())
      return ProductDocumentSuccessorStatus::Rejected;
  }
  tree = std::move(created.model);
  return ProductDocumentSuccessorStatus::Prepared;
}

ProductDocumentSuccessorStatus
MainWindow::prepareAssemblyVisibilityProjection(
    const ProductDocument& product,
    std::optional<OccurrencePath> selectionToRestore,
    std::unique_ptr<PreparedAssemblyProjection>& projection,
    QModelIndex& selectionIndex) noexcept {
  projection.reset();
  selectionIndex = {};
  if (!viewport_ || !viewport_->assemblySession() ||
      viewport_->assemblySession()->isolationActive())
    return ProductDocumentSuccessorStatus::Rejected;
  try {
    const auto* incumbent = viewport_->assemblySession();
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto preparedSession =
        AssemblyViewportSession::prepareVisibilityProjection(
            product, *incumbent, profile, assemblyPreparationFailurePoint_);
    if (!preparedSession)
      return preparedSession.status ==
                     AssemblyViewportSessionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    const auto failurePoint = assemblyTreeResourceFailureForTesting_
                                  ? AssemblyTreeModelFailurePoint::ResourceFailure
                                  : AssemblyTreeModelFailurePoint::None;
    auto tree = AssemblyTreeModel::createTyped(
        product, preparedSession.session->epoch(),
        preparedSession.session->publicationHandle(), nullptr, failurePoint,
        std::nullopt, &linkedPartRuntimeStates_);
    if (!tree)
      return tree.status == AssemblyTreeModelCreateStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;

    std::optional<AssemblyOccurrenceSelection> desired;
    if (selectionToRestore) {
      desired = AssemblyOccurrenceSelection{
          preparedSession.session->publicationHandle(),
          {preparedSession.session->epoch(), product.rootDefinitionId(),
           *selectionToRestore}};
      selectionIndex = tree.model->indexForOccurrence(*desired);
      if (!selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    auto preparedSelection =
        preparedSession.session->prepareSelectedOccurrence(desired);
    if (preparedSelection.status != PrepareAssemblySelectionStatus::Prepared)
      return preparedSelection.status ==
                     PrepareAssemblySelectionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    preparedSession.session->installPreparedSelectedOccurrence(
        std::move(preparedSelection.state));

    auto candidate = std::make_unique<PreparedAssemblyProjection>();
    candidate->session = std::move(preparedSession.session);
    candidate->treeModel = std::move(tree.model);
    candidate->epoch = incumbent->epoch();
    projection = std::move(candidate);
    return ProductDocumentSuccessorStatus::Prepared;
  } catch (const std::bad_alloc&) {
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

ProductDocumentSuccessorStatus
MainWindow::prepareAssemblyGroundingProjection(
    const ProductDocument& product,
    std::optional<OccurrencePath> selectionToRestore,
    std::unique_ptr<PreparedAssemblyProjection>& projection,
    QModelIndex& selectionIndex) noexcept {
  projection.reset();
  selectionIndex = {};
  if (!viewport_ || !viewport_->assemblySession() ||
      viewport_->assemblySession()->isolationActive())
    return ProductDocumentSuccessorStatus::Rejected;
  try {
    const auto* incumbent = viewport_->assemblySession();
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto preparedSession =
        AssemblyViewportSession::prepareGroundingProjection(
            product, *incumbent, profile, assemblyPreparationFailurePoint_);
    if (!preparedSession)
      return preparedSession.status ==
                     AssemblyViewportSessionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    const auto failurePoint = assemblyTreeResourceFailureForTesting_
                                  ? AssemblyTreeModelFailurePoint::ResourceFailure
                                  : AssemblyTreeModelFailurePoint::None;
    auto tree = AssemblyTreeModel::createTyped(
        product, preparedSession.session->epoch(),
        preparedSession.session->publicationHandle(), nullptr, failurePoint,
        std::nullopt, &linkedPartRuntimeStates_);
    if (!tree)
      return tree.status == AssemblyTreeModelCreateStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;

    std::optional<AssemblyOccurrenceSelection> desired;
    if (selectionToRestore) {
      desired = AssemblyOccurrenceSelection{
          preparedSession.session->publicationHandle(),
          {preparedSession.session->epoch(), product.rootDefinitionId(),
           *selectionToRestore}};
      selectionIndex = tree.model->indexForOccurrence(*desired);
      if (!selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    auto preparedSelection =
        preparedSession.session->prepareSelectedOccurrence(desired);
    if (preparedSelection.status != PrepareAssemblySelectionStatus::Prepared)
      return preparedSelection.status ==
                     PrepareAssemblySelectionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    preparedSession.session->installPreparedSelectedOccurrence(
        std::move(preparedSelection.state));

    auto candidate = std::make_unique<PreparedAssemblyProjection>();
    candidate->session = std::move(preparedSession.session);
    candidate->treeModel = std::move(tree.model);
    candidate->epoch = incumbent->epoch();
    projection = std::move(candidate);
    return ProductDocumentSuccessorStatus::Prepared;
  } catch (const std::bad_alloc&) {
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

ProductDocumentSuccessorStatus
MainWindow::prepareAssemblySuppressionProjection(
    const ProductDocument& product,
    std::optional<OccurrencePath> selectionToRestore,
    std::unique_ptr<PreparedAssemblyProjection>& projection,
    QModelIndex& selectionIndex) noexcept {
  projection.reset();
  selectionIndex = {};
  if (!viewport_ || !viewport_->assemblySession() ||
      viewport_->assemblySession()->isolationActive())
    return ProductDocumentSuccessorStatus::Rejected;
  try {
    const auto* incumbent = viewport_->assemblySession();
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto preparedSession =
        AssemblyViewportSession::prepareSuppressionProjection(
            product, *incumbent, profile, assemblyPreparationFailurePoint_);
    if (!preparedSession)
      return preparedSession.status ==
                     AssemblyViewportSessionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    const auto failurePoint = assemblyTreeResourceFailureForTesting_
                                  ? AssemblyTreeModelFailurePoint::ResourceFailure
                                  : AssemblyTreeModelFailurePoint::None;
    auto tree = AssemblyTreeModel::createTyped(
        product, preparedSession.session->epoch(),
        preparedSession.session->publicationHandle(), nullptr, failurePoint,
        std::nullopt, &linkedPartRuntimeStates_);
    if (!tree)
      return tree.status == AssemblyTreeModelCreateStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;

    std::optional<AssemblyOccurrenceSelection> desired;
    if (selectionToRestore) {
      desired = AssemblyOccurrenceSelection{
          preparedSession.session->publicationHandle(),
          {preparedSession.session->epoch(), product.rootDefinitionId(),
           *selectionToRestore}};
      selectionIndex = tree.model->indexForOccurrence(*desired);
      if (!selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    auto preparedSelection =
        preparedSession.session->prepareSelectedOccurrence(desired);
    if (preparedSelection.status != PrepareAssemblySelectionStatus::Prepared)
      return preparedSelection.status ==
                     PrepareAssemblySelectionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    preparedSession.session->installPreparedSelectedOccurrence(
        std::move(preparedSelection.state));

    auto candidate = std::make_unique<PreparedAssemblyProjection>();
    candidate->session = std::move(preparedSession.session);
    candidate->treeModel = std::move(tree.model);
    candidate->epoch = incumbent->epoch();
    projection = std::move(candidate);
    return ProductDocumentSuccessorStatus::Prepared;
  } catch (const std::bad_alloc&) {
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

ProductDocumentSuccessorStatus
MainWindow::prepareAssemblyDuplicateProjection(
    const ProductDocument& product,
    std::optional<OccurrencePath> selectionToRestore,
    std::unique_ptr<PreparedAssemblyProjection>& projection,
    QModelIndex& selectionIndex) noexcept {
  projection.reset();
  selectionIndex = {};
  if (!viewport_ || !viewport_->assemblySession() ||
      viewport_->assemblySession()->isolationActive())
    return ProductDocumentSuccessorStatus::Rejected;
  try {
    const auto* incumbent = viewport_->assemblySession();
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto preparedSession =
        AssemblyViewportSession::prepareDuplicateProjection(
            product, *incumbent, profile, assemblyPreparationFailurePoint_);
    if (!preparedSession)
      return preparedSession.status ==
                     AssemblyViewportSessionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    const auto failurePoint = assemblyTreeResourceFailureForTesting_
                                  ? AssemblyTreeModelFailurePoint::ResourceFailure
                                  : AssemblyTreeModelFailurePoint::None;
    auto tree = AssemblyTreeModel::createTyped(
        product, preparedSession.session->epoch(),
        preparedSession.session->publicationHandle(), nullptr, failurePoint,
        std::nullopt, &linkedPartRuntimeStates_);
    if (!tree)
      return tree.status == AssemblyTreeModelCreateStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;

    std::optional<AssemblyOccurrenceSelection> desired;
    if (selectionToRestore) {
      desired = AssemblyOccurrenceSelection{
          preparedSession.session->publicationHandle(),
          {preparedSession.session->epoch(), product.rootDefinitionId(),
           *selectionToRestore}};
      selectionIndex = tree.model->indexForOccurrence(*desired);
      if (!selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    auto preparedSelection =
        preparedSession.session->prepareSelectedOccurrence(desired);
    if (preparedSelection.status != PrepareAssemblySelectionStatus::Prepared)
      return preparedSelection.status ==
                     PrepareAssemblySelectionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    preparedSession.session->installPreparedSelectedOccurrence(
        std::move(preparedSelection.state));

    auto candidate = std::make_unique<PreparedAssemblyProjection>();
    candidate->session = std::move(preparedSession.session);
    candidate->treeModel = std::move(tree.model);
    candidate->epoch = incumbent->epoch();
    projection = std::move(candidate);
    return ProductDocumentSuccessorStatus::Prepared;
  } catch (const std::bad_alloc&) {
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

ProductDocumentSuccessorStatus
MainWindow::prepareAssemblyRemoveProjection(
    const ProductDocument& product,
    std::optional<OccurrencePath> selectionToRestore,
    std::unique_ptr<PreparedAssemblyProjection>& projection,
    QModelIndex& selectionIndex) noexcept {
  projection.reset();
  selectionIndex = {};
  const AssemblyViewportSession* incumbent =
      definitionEditContext_ ? suspendedAssemblySession_.get()
                             : (viewport_ ? viewport_->assemblySession()
                                          : nullptr);
  if (!incumbent || incumbent->isolationActive())
    return ProductDocumentSuccessorStatus::Rejected;
  try {
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto preparedSession = AssemblyViewportSession::prepareRemoveProjection(
        product, *incumbent, profile, assemblyPreparationFailurePoint_);
    if (!preparedSession)
      return preparedSession.status ==
                     AssemblyViewportSessionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    const auto failurePoint = assemblyTreeResourceFailureForTesting_
                                  ? AssemblyTreeModelFailurePoint::ResourceFailure
                                  : AssemblyTreeModelFailurePoint::None;
    auto tree = AssemblyTreeModel::createTyped(
        product, preparedSession.session->epoch(),
        preparedSession.session->publicationHandle(), nullptr, failurePoint,
        std::nullopt, &linkedPartRuntimeStates_);
    if (!tree)
      return tree.status == AssemblyTreeModelCreateStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;

    std::optional<AssemblyOccurrenceSelection> desired;
    if (selectionToRestore) {
      desired = AssemblyOccurrenceSelection{
          preparedSession.session->publicationHandle(),
          {preparedSession.session->epoch(), product.rootDefinitionId(),
           *selectionToRestore}};
      selectionIndex = tree.model->indexForOccurrence(*desired);
      if (!selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    auto preparedSelection =
        preparedSession.session->prepareSelectedOccurrence(desired);
    if (preparedSelection.status != PrepareAssemblySelectionStatus::Prepared)
      return preparedSelection.status ==
                     PrepareAssemblySelectionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    preparedSession.session->installPreparedSelectedOccurrence(
        std::move(preparedSelection.state));

    auto candidate = std::make_unique<PreparedAssemblyProjection>();
    candidate->session = std::move(preparedSession.session);
    candidate->treeModel = std::move(tree.model);
    candidate->epoch = incumbent->epoch();
    projection = std::move(candidate);
    return ProductDocumentSuccessorStatus::Prepared;
  } catch (const std::bad_alloc&) {
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

ProductDocumentSuccessorStatus
MainWindow::prepareAssemblyDefinitionProjection(
    const ProductDocument& product,
    std::optional<OccurrencePath> selectionToRestore,
    std::optional<DefinitionId> definitionSelectionToRestore,
    std::unique_ptr<PreparedAssemblyProjection>& projection,
    QModelIndex& selectionIndex) noexcept {
  projection.reset();
  selectionIndex = {};
  const AssemblyViewportSession* incumbent =
      definitionEditContext_ ? suspendedAssemblySession_.get()
                             : (viewport_ ? viewport_->assemblySession()
                                          : nullptr);
  if (!incumbent || incumbent->isolationActive())
    return ProductDocumentSuccessorStatus::Rejected;
  try {
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto preparedSession = AssemblyViewportSession::prepareDefinitionProjection(
        product, *incumbent, profile, assemblyPreparationFailurePoint_);
    if (!preparedSession)
      return preparedSession.status ==
                     AssemblyViewportSessionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    const auto failurePoint = assemblyTreeResourceFailureForTesting_
                                  ? AssemblyTreeModelFailurePoint::ResourceFailure
                                  : AssemblyTreeModelFailurePoint::None;
    auto tree = AssemblyTreeModel::createTyped(
        product, preparedSession.session->epoch(),
        preparedSession.session->publicationHandle(), nullptr, failurePoint,
        std::nullopt, &linkedPartRuntimeStates_);
    if (!tree)
      return tree.status == AssemblyTreeModelCreateStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;

    std::optional<AssemblyOccurrenceSelection> desired;
    if (selectionToRestore) {
      desired = AssemblyOccurrenceSelection{
          preparedSession.session->publicationHandle(),
          {preparedSession.session->epoch(), product.rootDefinitionId(),
           *selectionToRestore}};
      selectionIndex = tree.model->indexForOccurrence(*desired);
      if (!selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    auto preparedSelection =
        preparedSession.session->prepareSelectedOccurrence(desired);
    if (preparedSelection.status != PrepareAssemblySelectionStatus::Prepared)
      return preparedSelection.status ==
                     PrepareAssemblySelectionStatus::ResourceFailure
                 ? ProductDocumentSuccessorStatus::ResourceFailure
                 : ProductDocumentSuccessorStatus::Rejected;
    preparedSession.session->installPreparedSelectedOccurrence(
        std::move(preparedSelection.state));

    std::optional<AssemblyOccurrenceSelection> desiredHover;
    if (const auto& incumbentHover = incumbent->hoveredOccurrence()) {
      desiredHover = AssemblyOccurrenceSelection{
          preparedSession.session->publicationHandle(),
          {preparedSession.session->epoch(), product.rootDefinitionId(),
           incumbentHover->occurrence.path}};
    }
    if (!preparedSession.session->setHoveredOccurrence(desiredHover))
      return ProductDocumentSuccessorStatus::Rejected;

    if (definitionSelectionToRestore) {
      selectionIndex =
          tree.model->indexForDefinition(*definitionSelectionToRestore);
      if (!selectionIndex.isValid())
        return ProductDocumentSuccessorStatus::Rejected;
    }
    auto candidate = std::make_unique<PreparedAssemblyProjection>();
    candidate->session = std::move(preparedSession.session);
    candidate->treeModel = std::move(tree.model);
    candidate->epoch = incumbent->epoch();
    projection = std::move(candidate);
    return ProductDocumentSuccessorStatus::Prepared;
  } catch (const std::bad_alloc&) {
    return ProductDocumentSuccessorStatus::ResourceFailure;
  } catch (...) {
    return ProductDocumentSuccessorStatus::Rejected;
  }
}

bool MainWindow::prepareAssemblyIsolationProjection(
    const std::optional<AssemblyOccurrenceIdentity>& occurrence,
    std::optional<PreparedAssemblyIsolationState>& prepared,
    std::unique_ptr<AssemblyTreeModel>& tree,
    QModelIndex& selectionIndex) noexcept {
  prepared.reset();
  tree.reset();
  selectionIndex = {};
  if (!viewport_ || !viewport_->assemblySession()) return false;
  try {
    auto isolated = viewport_->assemblySession()->prepareIsolation(
        occurrence, assemblyIsolationFailurePointForTesting_);
    if (!isolated) return false;
    const std::optional<OccurrencePath> isolatedId =
        occurrence ? std::optional<OccurrencePath>{occurrence->path}
                   : std::nullopt;
    const auto failurePoint = assemblyTreeResourceFailureForTesting_
                                  ? AssemblyTreeModelFailurePoint::ResourceFailure
                                  : AssemblyTreeModelFailurePoint::None;
    auto created = AssemblyTreeModel::createTyped(
        productDocument_, viewport_->assemblySession()->epoch(),
        viewport_->assemblySession()->publicationHandle(), nullptr,
        failurePoint, isolatedId, &linkedPartRuntimeStates_);
    if (!created) return false;
    const auto& selected = isolated.state->selection.selection;
    if (selected) {
      selectionIndex = created.model->indexForOccurrence(*selected);
      if (!selectionIndex.isValid()) return false;
    }
    prepared = std::move(*isolated.state);
    tree = std::move(created.model);
    return true;
  } catch (...) {
    return false;
  }
}

void MainWindow::installAssemblyTreeProjection(
    std::unique_ptr<AssemblyTreeModel> tree,
    const QModelIndex& selectionIndex,
    std::optional<PreparedAssemblySelectionState> preparedSelection) noexcept {
  if (!tree || !assemblyTreeView_ || !viewport_ ||
      !viewport_->assemblySession())
    return;
  QObject::disconnect(assemblyTreeSelectionConnection_);
  assemblyTreeSelectionConnection_ = {};
  assemblyTreeView_->setModel(nullptr);
  delete assemblyTreeModel_;
  assemblyTreeModel_ = tree.release();
  assemblyTreeModel_->setParent(assemblyTreeView_);
  assemblyTreeView_->setModel(assemblyTreeModel_);
  assemblyTreeView_->expandAll();
  assemblyTreeSelectionConnection_ = connect(
      assemblyTreeView_->selectionModel(),
      &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        selectAssemblyTreeIndex(current);
      });
  const QSignalBlocker selectionSignals(assemblyTreeView_->selectionModel());
  if (preparedSelection)
    viewport_->installPreparedAssemblySelection(
        std::move(*preparedSelection));

  // selectionIndex was created by this exact tree during successor prepare.
  // The viewport selection/hover/highlights are deliberately untouched.
  if (selectionIndex.isValid())
    assemblyTreeView_->setCurrentIndex(selectionIndex);
  else
    assemblyTreeView_->setCurrentIndex({});
  updateAssemblyRenameAvailability();
}

void MainWindow::installAssemblyVisibilityProjection(
    std::unique_ptr<PreparedAssemblyProjection> projection,
    const QModelIndex& selectionIndex) noexcept {
  if (!projection || !projection->session || !projection->treeModel ||
      !viewport_ || !viewport_->assemblySession() ||
      projection->epoch != viewport_->assemblySession()->epoch())
    return;
  auto tree = std::move(projection->treeModel);
  viewport_->installAssemblyVisibilitySession(std::move(projection->session));
  installAssemblyTreeProjection(std::move(tree), selectionIndex, std::nullopt);
}

void MainWindow::installAssemblyGroundingProjection(
    std::unique_ptr<PreparedAssemblyProjection> projection,
    const QModelIndex& selectionIndex) noexcept {
  if (!projection || !projection->session || !projection->treeModel ||
      !viewport_ || !viewport_->assemblySession() ||
      projection->epoch != viewport_->assemblySession()->epoch())
    return;
  auto tree = std::move(projection->treeModel);
  viewport_->installAssemblyGroundingSession(std::move(projection->session));
  installAssemblyTreeProjection(std::move(tree), selectionIndex, std::nullopt);
}

void MainWindow::installAssemblySuppressionProjection(
    std::unique_ptr<PreparedAssemblyProjection> projection,
    const QModelIndex& selectionIndex) noexcept {
  if (!projection || !projection->session || !projection->treeModel ||
      !viewport_ || !viewport_->assemblySession() ||
      projection->epoch != viewport_->assemblySession()->epoch())
    return;
  auto tree = std::move(projection->treeModel);
  viewport_->installAssemblySuppressionSession(std::move(projection->session));
  installAssemblyTreeProjection(std::move(tree), selectionIndex, std::nullopt);
}

void MainWindow::installAssemblyDuplicateProjection(
    std::unique_ptr<PreparedAssemblyProjection> projection,
    const QModelIndex& selectionIndex) noexcept {
  if (!projection || !projection->session || !projection->treeModel ||
      !viewport_ || !viewport_->assemblySession() ||
      projection->epoch != viewport_->assemblySession()->epoch())
    return;
  auto tree = std::move(projection->treeModel);
  viewport_->installAssemblyDuplicateSession(std::move(projection->session));
  installAssemblyTreeProjection(std::move(tree), selectionIndex, std::nullopt);
}

void MainWindow::installAssemblyRemoveProjection(
    std::unique_ptr<PreparedAssemblyProjection> projection,
    const QModelIndex& selectionIndex) noexcept {
  if (definitionEditContext_) {
    installAssemblyDefinitionProjection(std::move(projection), selectionIndex);
    return;
  }
  if (!projection || !projection->session || !projection->treeModel ||
      !viewport_ || !viewport_->assemblySession() ||
      projection->epoch != viewport_->assemblySession()->epoch())
    return;
  auto tree = std::move(projection->treeModel);
  viewport_->installAssemblyRemoveSession(std::move(projection->session));
  installAssemblyTreeProjection(std::move(tree), selectionIndex, std::nullopt);
}

void MainWindow::installAssemblyDefinitionProjection(
    std::unique_ptr<PreparedAssemblyProjection> projection,
    const QModelIndex& selectionIndex) noexcept {
  if (!projection || !projection->session || !projection->treeModel)
    return;
  if (!definitionEditContext_) {
    if (!viewport_ || !viewport_->assemblySession() ||
        projection->epoch != viewport_->assemblySession()->epoch())
      return;
    auto tree = std::move(projection->treeModel);
    viewport_->installAssemblyDefinitionSession(
        std::move(projection->session));
    installAssemblyTreeProjection(std::move(tree), selectionIndex,
                                  std::nullopt);
    return;
  }
  if (!suspendedAssemblySession_ ||
      projection->epoch != suspendedAssemblySession_->epoch())
    return;
  suspendedAssemblySession_ = std::move(projection->session);
  QObject::disconnect(assemblyTreeSelectionConnection_);
  assemblyTreeSelectionConnection_ = {};
  if (assemblyTreeView_) assemblyTreeView_->setModel(nullptr);
  delete assemblyTreeModel_;
  assemblyTreeModel_ = projection->treeModel.release();
  assemblyTreeModel_->setParent(assemblyTreeView_);
  assemblyTreeView_->setModel(assemblyTreeModel_);
  assemblyTreeView_->expandAll();
  assemblyTreeSelectionConnection_ = connect(
      assemblyTreeView_->selectionModel(),
      &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        selectAssemblyTreeIndex(current);
      });
  const QSignalBlocker blocker(assemblyTreeView_->selectionModel());
  QModelIndex selected = selectionIndex;
  if (!selected.isValid())
    selected = assemblyTreeModel_->indexForDefinition(
        definitionEditContext_->targetDefinitionId);
  assemblyTreeView_->setCurrentIndex(selected);
}

void MainWindow::installAssemblyIsolationProjection(
    std::optional<PreparedAssemblyIsolationState> prepared,
    std::unique_ptr<AssemblyTreeModel> tree,
    const QModelIndex& selectionIndex) noexcept {
  if (!prepared || !tree || !viewport_ ||
      !viewport_->installPreparedAssemblyIsolation(std::move(*prepared)))
    return;
  installAssemblyTreeProjection(std::move(tree), selectionIndex, std::nullopt);
  updateUndoAvailability();
}

void MainWindow::installAssemblyProjection(
    std::unique_ptr<PreparedAssemblyProjection> projection) noexcept {
  if (!projection || !projection->session || !projection->treeModel ||
      !projection->epoch.isValid())
    return;
  QObject::disconnect(assemblyTreeSelectionConnection_);
  assemblyTreeSelectionConnection_ = {};
  if (assemblyTreeView_) assemblyTreeView_->setModel(nullptr);
  delete assemblyTreeModel_;
  assemblyTreeModel_ = projection->treeModel.release();
  assemblyTreeModel_->setParent(assemblyTreeView_);
  assemblyTreeView_->setModel(assemblyTreeModel_);
  assemblyTreeView_->expandAll();
  assemblyTreeSelectionConnection_ = connect(
      assemblyTreeView_->selectionModel(),
      &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        selectAssemblyTreeIndex(current);
      });
  const QModelIndex selectionIndex = projection->selectionIndex;
  assemblyPresentationEpochClock_ = projection->epoch.value();
  viewport_->installAssemblySession(std::move(projection->session));
  const QSignalBlocker blocker(assemblyTreeView_->selectionModel());
  assemblyTreeView_->setCurrentIndex(selectionIndex);
  viewport_->update();
  updateAssemblyRenameAvailability();
}

void MainWindow::createNewAssembly() {
  QPointer<MainWindow> receiver(this);
  const auto confirmation = confirmProjectReplacement(*this);
  if (!receiver || confirmation != ProjectReplacementConfirmation::Proceed)
    return;
  auto staged = projectApplicationService_.stageNewAssembly();
  if (!staged.succeeded()) return;
  auto prepared = prepareAssemblyWorkspace(std::move(*staged.staged));
  if (!prepared) return;
  commitAssemblyReplacement(std::move(prepared), QString{});
}

void MainWindow::enterAssemblyDefinitionEdit() {
  updateAssemblyRenameAvailability();
  if (!assemblyEditDefinitionAction_ ||
      !assemblyEditDefinitionAction_->isEnabled() ||
      definitionEditContext_ || !assemblyTreeModel_ || !assemblyTreeView_ ||
      !viewport_ || !viewport_->assemblySession() ||
      assemblyCommandHistory_.isBusy() ||
      assemblyInsertController_.active() || assemblyMateActive())
    return;
  const auto address = assemblyTreeModel_->addressForIndex(
      assemblyTreeView_->currentIndex());
  if (!address ||
      address->get().kind != AssemblyTreeNodeKind::PartDefinition)
    return;
  const DefinitionId target = address->get().definitionId;
  const auto* part = productDocument_.definitions().findPart(target);
  if (!part) return;
  const DefinitionStateId state = part->stateId();
  const ViewportNavigationState navigation = viewport_->navigationState();
  const auto floor = assemblyCommandHistory_.currentRevision();
  AssemblyDefinitionEditContext context{
      productDocument_.incarnation(), productDocument_.rootDefinitionId(),
      target, state, floor, target, navigation.yawDeg,
      navigation.pitchDeg, navigation.zoom, navigation.pan,
      previewDocumentGeneration_ + 1};
  auto suspended = viewport_->suspendAssemblySession();
  if (!suspended) return;
  definitionEditContext_.emplace(std::move(context));
  suspendedAssemblySession_ = std::move(suspended);
  invalidatePreviewUpdates();
  modelHistory_.clear(true);
  viewport_->resetScenePreservingSuspendedAssemblyResources();
  viewport_->setBox(activePartDocument().box());
  sketchViews_.clear();
  for (const auto& item : activePartDocument().sketches()) {
    sketchViews_.push_back({item.id});
    viewport_->addSketch(item.id, item.geometry,
                         sketchPresentationLabel(item), item.placement);
  }
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  refreshBodyViewFromDocument();
  workspaceStack_->setCurrentWidget(viewport_);
  applyProjectKind(EditorProjectKind::AssemblyReadOnly);
  definitionEditBreadcrumb_->setText(
      QString::fromUtf8("Сборка %1 › Деталь %2")
          .arg(definitionEditContext_->rootDefinitionId.value())
          .arg(target.value()));
  definitionEditBreadcrumb_->show();
  definitionEditBackButton_->show();
  statusBar()->showMessage(QString::fromUtf8("Редактирование общей детали"),
                           3000);
  updateUndoAvailability();
}

void MainWindow::leaveAssemblyDefinitionEdit() {
  if (!definitionEditContext_ || !suspendedAssemblySession_ || !viewport_)
    return;
  const bool legacyPartDesignActive =
      selectedExtrusionSource_.has_value() ||
      (extrusionDock_ && extrusionDock_->isVisible());
  const bool referenceImageTransaction =
      !pendingReferenceImagePath_.isEmpty() ||
      pendingReferenceImagePixelSize_.isValid() ||
      referenceImageEditBefore_.has_value() ||
      referenceImageEditSelectionBefore_.has_value();
  if (partDesignCoordinator_.activeTool() != PartDesignToolKind::None ||
      partDesignCoordinator_.commitPending() ||
      partDesignCoordinator_.isReselecting() || editUiTransaction_ ||
      pendingNoChangeUiRestore_ || legacyPartDesignActive ||
      referenceImageTransaction || viewport_->referenceImageManipulatorActive() ||
      (workspaceStack_ && workspaceStack_->currentWidget() != viewport_)) {
    statusBar()->showMessage(
        QString::fromUtf8("Завершите или отмените активный инструмент"), 3000);
    return;
  }
  const DefinitionId target = definitionEditContext_->targetDefinitionId;
  const ViewportNavigationState navigation{
      definitionEditContext_->assemblyYawDeg,
      definitionEditContext_->assemblyPitchDeg,
      definitionEditContext_->assemblyZoom,
      definitionEditContext_->assemblyPan};
  invalidatePreviewUpdates();
  modelHistory_.clear(true);
  auto session = std::move(suspendedAssemblySession_);
  definitionEditContext_.reset();
  viewport_->resumeAssemblySession(std::move(session));
  viewport_->restoreNavigationState(navigation);
  applyProjectKind(EditorProjectKind::AssemblyReadOnly);
  if (assemblyTreeModel_ && assemblyTreeView_) {
    const QModelIndex index = assemblyTreeModel_->indexForDefinition(target);
    const QSignalBlocker blocker(assemblyTreeView_->selectionModel());
    assemblyTreeView_->setCurrentIndex(index);
  }
  if (definitionEditBreadcrumb_) definitionEditBreadcrumb_->hide();
  if (definitionEditBackButton_) definitionEditBackButton_->hide();
  statusBar()->showMessage(QString::fromUtf8("Сборка"), 2000);
  updateAssemblyRenameAvailability();
  updateUndoAvailability();
}

std::optional<DefinitionId>
MainWindow::selectedLinkedDefinitionId() const noexcept {
  if (projectKind_ != EditorProjectKind::AssemblyReadOnly ||
      definitionEditContext_ || !assemblyTreeModel_ || !assemblyTreeView_)
    return std::nullopt;
  const auto address = assemblyTreeModel_->addressForIndex(
      assemblyTreeView_->currentIndex());
  if (!address) return std::nullopt;
  const DefinitionId id = address->get().definitionId;
  return productDocument_.partLinkedBinding(id)
             ? std::optional<DefinitionId>{id}
             : std::nullopt;
}

void MainWindow::updateLinkedPartLifecycleAvailability() noexcept {
  const bool quiescent =
      projectKind_ == EditorProjectKind::AssemblyReadOnly &&
      !windowFilePath().isEmpty() && !definitionEditContext_ &&
      !assemblyInsertController_.active() && !assemblyMateActive() &&
      !assemblyCommandHistory_.isBusy() && assemblyRenameDialog_.isNull() &&
      !assemblySaveAsChooserActive_ && !linkedPartRepairChooserActive_ &&
      viewport_ && viewport_->assemblySession() &&
      !viewport_->assemblySession()->isolationActive();
  const bool enabled = quiescent && selectedLinkedDefinitionId().has_value();
  if (linkedPartReloadAction_) linkedPartReloadAction_->setEnabled(enabled);
  if (linkedPartRepairAction_) linkedPartRepairAction_->setEnabled(enabled);
}

void MainWindow::reloadSelectedLinkedPart() {
  updateLinkedPartLifecycleAvailability();
  if (!linkedPartReloadAction_ || !linkedPartReloadAction_->isEnabled()) return;
  const auto target = selectedLinkedDefinitionId();
  if (!target) return;
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
  AssemblyCommandSemanticState successorState;
  try {
    auto captured = captureAssemblyCommandSemanticState();
    if (!captured) return;
    beforeState = std::move(*captured);
    afterState = beforeState;
    successorState = beforeState;
  } catch (...) {
    return;
  }
  const auto found = linkedPartRuntimeStates_.find(*target);
  const std::optional<LinkedPartRuntimeState> previous =
      found == linkedPartRuntimeStates_.end()
          ? std::nullopt
          : std::optional<LinkedPartRuntimeState>{found->second};
  linkedPartRuntimeStates_[*target] = {LinkedPartRuntimeStatus::Exact, {}};

  std::unique_ptr<PreparedAssemblyProjection> successor;
  std::unique_ptr<PreparedAssemblyProjection> visibilitySuccessor;
  std::unique_ptr<PreparedAssemblyProjection> groundingSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> suppressionSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> duplicateSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> removeSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> definitionSuccessor;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex treeSelectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, successor, visibilitySuccessor, groundingSuccessor,
      suppressionSuccessor, duplicateSuccessor, removeSuccessor,
      definitionSuccessor, treeOnly, treeSelectionIndex, preparedSelection,
      std::move(successorState));
  const auto result = LinkedPartLifecycleService::reload(
      productDocument_, *target,
      linkedPolicyForAssemblyPath(windowFilePath()), assemblyCommandHistory_,
      prepare, std::move(beforeState), std::move(afterState));
  if (result.status != LinkedPartLifecycleStatus::Committed) {
    if (previous)
      linkedPartRuntimeStates_[*target] = *previous;
    else
      linkedPartRuntimeStates_.erase(*target);
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось обновить связанную деталь"), 5000);
    updateLinkedPartLifecycleAvailability();
    return;
  }
  installAssemblyDefinitionProjection(std::move(definitionSuccessor),
                                      treeSelectionIndex);
  setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
  updateUndoAvailability();
  updateLinkedPartLifecycleAvailability();
  statusBar()->showMessage(
      QString::fromUtf8("Связанная деталь обновлена"), 3000);
}

void MainWindow::repairSelectedLinkedPart() {
  const LinkedPartRepairSourceChooser chooseSource =
      [this](const QString& suggestedRoot) -> std::optional<QString> {
    const QString selected = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("Исправить ссылку на деталь"), suggestedRoot,
        QString::fromUtf8("Проекты СОЛИДАР3D (*.solidar)"));
    return selected.isEmpty() ? std::nullopt
                              : std::optional<QString>{selected};
  };
  static_cast<void>(runLinkedPartRepairCommand(chooseSource));
}

bool MainWindow::runLinkedPartRepairCommand(
    const LinkedPartRepairSourceChooser& chooseSource,
    bool* repairAttempted) {
  if (repairAttempted) *repairAttempted = false;
  updateLinkedPartLifecycleAvailability();
  if (!linkedPartRepairAction_ || !linkedPartRepairAction_->isEnabled())
    return false;
  const auto target = selectedLinkedDefinitionId();
  if (!target) return false;
  const auto* binding = productDocument_.partLinkedBinding(*target);
  if (!binding) return false;
  const ProductIncarnationId expectedIncarnation =
      productDocument_.incarnation();
  const DefinitionId expectedRoot = productDocument_.rootDefinitionId();
  const DefinitionStateId expectedState =
      productDocument_.definitionStateId(*target);
  const LinkedPartBinding expectedBinding = *binding;
  const auto expectedHistory = assemblyCommandHistory_.currentRevision();
  const std::uint64_t expectedPresentation =
      assemblyPresentationEpochClock_;
  const QString expectedPath = windowFilePath();
  if (!viewport_ || !viewport_->assemblySession()) return false;
  const AssemblyScenePublicationHandle expectedPublication =
      viewport_->assemblySession()->publicationHandle();
  std::vector<std::pair<DefinitionId, DefinitionStateId>> expectedStates;
  for (const DefinitionId id : productDocument_.definitions().definitionIds())
    expectedStates.emplace_back(id, productDocument_.definitionStateId(id));
  QPointer<MainWindow> receiver(this);
  std::optional<QString> selected;
  linkedPartRepairChooserActive_ = true;
  updateLinkedPartLifecycleAvailability();
  try {
    selected = chooseSource(QFileInfo(expectedPath).absolutePath());
  } catch (...) {
    if (receiver) {
      receiver->linkedPartRepairChooserActive_ = false;
      receiver->updateLinkedPartLifecycleAvailability();
    }
    return false;
  }
  if (!receiver) return false;
  linkedPartRepairChooserActive_ = false;
  updateLinkedPartLifecycleAvailability();
  if (!selected || selected->isEmpty()) return false;
  bool statesCurrent =
      productDocument_.definitions().definitionIds().size() ==
      expectedStates.size();
  for (const auto& [id, state] : expectedStates)
    statesCurrent = statesCurrent &&
                    productDocument_.definitionStateId(id) == state;
  if (productDocument_.incarnation() != expectedIncarnation ||
      productDocument_.rootDefinitionId() != expectedRoot ||
      productDocument_.definitionStateId(*target) != expectedState ||
      productDocument_.partLinkedBinding(*target) == nullptr ||
      *productDocument_.partLinkedBinding(*target) != expectedBinding ||
      assemblyCommandHistory_.currentRevision() != expectedHistory ||
      assemblyPresentationEpochClock_ != expectedPresentation ||
      windowFilePath() != expectedPath ||
      !statesCurrent || selectedLinkedDefinitionId() != target ||
      definitionEditContext_ ||
      assemblyInsertController_.active() || assemblyMateActive() ||
      assemblyCommandHistory_.isBusy() ||
      !assemblyRenameDialog_.isNull() || assemblySaveAsChooserActive_ ||
      linkedPartRepairChooserActive_ || !viewport_ ||
      !viewport_->assemblySession() ||
      viewport_->assemblySession()->isolationActive() ||
      !viewport_->assemblySession()
           ->publicationHandle()
           .isSameCurrentPublication(expectedPublication)) {
    statusBar()->showMessage(
        QString::fromUtf8("Состояние сборки изменилось — исправление отменено"),
        5000);
    return false;
  }
  QDir root(QFileInfo(expectedPath).absolutePath());
  const QString relative =
      QDir::cleanPath(root.relativeFilePath(*selected));
#if defined(Q_OS_WIN)
  constexpr auto semantics = project::LinkedPathSemantics::Windows;
#else
  constexpr auto semantics = project::LinkedPathSemantics::Posix;
#endif
  if (!project::isSafeRelativeLinkedPathHint(relative, semantics)) {
    statusBar()->showMessage(
        QString::fromUtf8("Файл должен находиться внутри папки проекта"),
        5000);
    return false;
  }

  const auto oldRuntime = linkedPartRuntimeStates_.find(*target);
  AssemblyCommandSemanticState beforeState;
  AssemblyCommandSemanticState afterState;
  AssemblyCommandSemanticState successorState;
  try {
    auto captured = captureAssemblyCommandSemanticState();
    if (!captured) return false;
    beforeState = std::move(*captured);
    afterState = beforeState;
    successorState = beforeState;
  } catch (...) {
    return false;
  }
  const std::optional<LinkedPartRuntimeState> previous =
      oldRuntime == linkedPartRuntimeStates_.end()
          ? std::nullopt
          : std::optional<LinkedPartRuntimeState>{oldRuntime->second};
  linkedPartRuntimeStates_[*target] = {LinkedPartRuntimeStatus::Outdated, {}};
  std::unique_ptr<PreparedAssemblyProjection> successor;
  std::unique_ptr<PreparedAssemblyProjection> visibilitySuccessor;
  std::unique_ptr<PreparedAssemblyProjection> groundingSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> suppressionSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> duplicateSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> removeSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> definitionSuccessor;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex treeSelectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, successor, visibilitySuccessor, groundingSuccessor,
      suppressionSuccessor, duplicateSuccessor, removeSuccessor,
      definitionSuccessor, treeOnly, treeSelectionIndex, preparedSelection,
      std::move(successorState));
  bool resolvedOutdated = true;
  class RepairRuntimeStatusSuccessor final
      : public ProductDocumentSuccessor {
   public:
    RepairRuntimeStatusSuccessor(ProductDocumentSuccessor& inner,
                                 LinkedPartRuntimeStates& states,
                                 DefinitionId target,
                                 const bool& outdated) noexcept
        : inner_(inner), states_(states), target_(target),
          outdated_(outdated) {}

    ProductDocumentSuccessorStatus prepare(
        const ProductDocument& candidate,
        ProductDocumentSuccessorImpact impact) noexcept override {
      states_[target_] = {
          outdated_ ? LinkedPartRuntimeStatus::Outdated
                    : LinkedPartRuntimeStatus::Exact,
          {}};
      return inner_.prepare(candidate, impact);
    }

   private:
    ProductDocumentSuccessor& inner_;
    LinkedPartRuntimeStates& states_;
    DefinitionId target_{};
    const bool& outdated_;
  } statusPrepare(prepare, linkedPartRuntimeStates_, *target,
                  resolvedOutdated);
  if (repairAttempted) *repairAttempted = true;
  const auto result = LinkedPartLifecycleService::repair(
      productDocument_, *target, relative,
      linkedPolicyForAssemblyPath(expectedPath), assemblyCommandHistory_,
      statusPrepare, std::move(beforeState), std::move(afterState),
      &resolvedOutdated);
  if (result.status != LinkedPartLifecycleStatus::Committed) {
    if (previous)
      linkedPartRuntimeStates_[*target] = *previous;
    else
      linkedPartRuntimeStates_.erase(*target);
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось исправить ссылку"), 5000);
    updateLinkedPartLifecycleAvailability();
    return false;
  }
  linkedPartRuntimeStates_[*target] = {
      result.outdated ? LinkedPartRuntimeStatus::Outdated
                      : LinkedPartRuntimeStatus::Exact,
      {}};
  installAssemblyTreeProjection(std::move(treeOnly), treeSelectionIndex,
                                std::move(preparedSelection));
  setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
  updateUndoAvailability();
  updateLinkedPartLifecycleAvailability();
  statusBar()->showMessage(
      result.outdated
          ? QString::fromUtf8(
                "Ссылка исправлена; источник изменён, требуется обновление")
          : QString::fromUtf8("Ссылка исправлена"),
      5000);
  return true;
}

void MainWindow::updateAssemblyRenameAvailability() noexcept {
  const bool assembly =
      projectKind_ == EditorProjectKind::AssemblyReadOnly && viewport_ &&
      viewport_->assemblySession();
  const bool isolateActive =
      assembly && viewport_->assemblySession()->isolationActive();
  const bool quiescent = assembly && !assemblyInsertController_.active() &&
                         !assemblyMateActive() &&
                         !assemblyCommandHistory_.isBusy() &&
                         assemblyRenameDialog_.isNull();
  bool definitionSelected = false;
  if (quiescent && assemblyTreeModel_ && assemblyTreeView_) {
    const auto address = assemblyTreeModel_->addressForIndex(
        assemblyTreeView_->currentIndex());
    definitionSelected =
        address &&
        address->get().kind == AssemblyTreeNodeKind::PartDefinition &&
        productDocument_.definitions().findPart(
            address->get().definitionId) != nullptr &&
        productDocument_.partLinkedBinding(address->get().definitionId) ==
            nullptr;
  }
  bool selectedValid = quiescent;
  const ComponentOccurrence* occurrence = nullptr;
  if (selectedValid) {
    const auto& selected = viewport_->selectedAssemblyOccurrenceView();
    selectedValid = selected &&
                    selected->isCurrentFor(
                        viewport_->assemblySession()->publicationHandle()) &&
                    selected->occurrence.rootAssemblyDefinitionId ==
                        productDocument_.rootDefinitionId() &&
                    selected->occurrence.path.depth() == 1;
    if (selectedValid) {
      const auto resolved = productDocument_.resolveRootOccurrence(
          selected->occurrence.path);
      occurrence = resolved ? resolved.occurrence : nullptr;
      selectedValid = occurrence != nullptr;
    }
  }
  const bool authoredEnabled = selectedValid && !isolateActive;
  if (assemblyRenameButton_)
    assemblyRenameButton_->setEnabled(authoredEnabled);
  if (assemblyRenameAction_)
    assemblyRenameAction_->setEnabled(authoredEnabled);
  if (assemblyRenameShortcut_)
    assemblyRenameShortcut_->setEnabled(authoredEnabled);

  if (assemblyVisibilityAction_) {
    assemblyVisibilityAction_->setText(
        occurrence && !occurrence->flags().visible
            ? QString::fromUtf8("Показать экземпляр")
            : QString::fromUtf8("Скрыть экземпляр"));
    assemblyVisibilityAction_->setEnabled(authoredEnabled);
  }
  if (assemblyVisibilityButton_) {
    assemblyVisibilityButton_->setText(
        assemblyVisibilityAction_ ? assemblyVisibilityAction_->text()
                                  : QString{});
    assemblyVisibilityButton_->setEnabled(authoredEnabled);
  }
  if (assemblyVisibilityShortcut_)
    assemblyVisibilityShortcut_->setEnabled(authoredEnabled);

  if (assemblyGroundingAction_) {
    assemblyGroundingAction_->setText(
        occurrence && occurrence->flags().grounded
            ? QString::fromUtf8("Снять фиксацию экземпляра")
            : QString::fromUtf8("Зафиксировать экземпляр"));
    assemblyGroundingAction_->setEnabled(authoredEnabled);
  }
  if (assemblyGroundingButton_) {
    assemblyGroundingButton_->setText(
        assemblyGroundingAction_ ? assemblyGroundingAction_->text()
                                 : QString{});
    assemblyGroundingButton_->setEnabled(authoredEnabled);
  }
  if (assemblyGroundingShortcut_)
    assemblyGroundingShortcut_->setEnabled(authoredEnabled);

  if (assemblySuppressionAction_) {
    assemblySuppressionAction_->setText(
        occurrence && occurrence->flags().suppressed
            ? QString::fromUtf8("Снять подавление экземпляра")
            : QString::fromUtf8("Подавить экземпляр"));
    assemblySuppressionAction_->setEnabled(authoredEnabled);
  }
  if (assemblySuppressionButton_) {
    assemblySuppressionButton_->setText(
        assemblySuppressionAction_ ? assemblySuppressionAction_->text()
                                   : QString{});
    assemblySuppressionButton_->setEnabled(authoredEnabled);
  }
  if (assemblySuppressionShortcut_)
    assemblySuppressionShortcut_->setEnabled(authoredEnabled);

  const bool duplicateEnabled =
      authoredEnabled && occurrence &&
      productDocument_.definitions().findPart(
          occurrence->targetDefinitionId()) != nullptr &&
      productDocument_.lowestFreeRootOccurrenceId().has_value();
  if (assemblyDuplicateAction_)
    assemblyDuplicateAction_->setEnabled(duplicateEnabled);
  if (assemblyDuplicateButton_)
    assemblyDuplicateButton_->setEnabled(duplicateEnabled);
  if (assemblyDuplicateShortcut_)
    assemblyDuplicateShortcut_->setEnabled(duplicateEnabled);

  const bool removeEnabled =
      authoredEnabled && occurrence &&
      productDocument_.definitions().findPart(
          occurrence->targetDefinitionId()) != nullptr;
  if (assemblyRemoveAction_)
    assemblyRemoveAction_->setEnabled(removeEnabled);
  if (assemblyRemoveButton_)
    assemblyRemoveButton_->setEnabled(removeEnabled);
  if (assemblyRemoveShortcut_)
    assemblyRemoveShortcut_->setEnabled(removeEnabled);
  if (assemblyEditDefinitionAction_)
    assemblyEditDefinitionAction_->setEnabled(definitionSelected &&
                                              !isolateActive);
  if (assemblyEditDefinitionButton_)
    assemblyEditDefinitionButton_->setEnabled(definitionSelected &&
                                              !isolateActive);

  bool isolateEnabled = isolateActive && quiescent;
  if (!isolateActive && quiescent) {
    const auto& selected = viewport_->selectedAssemblyOccurrenceView();
    isolateEnabled =
        selected &&
        selected->isCurrentFor(
            viewport_->assemblySession()->publicationHandle()) &&
        selected->occurrence.rootAssemblyDefinitionId ==
            productDocument_.rootDefinitionId() &&
        static_cast<bool>(
            productDocument_.resolveOccurrence(selected->occurrence.path)) &&
        viewport_->assemblySession()->canIsolate(selected->occurrence);
  }
  const QString isolateText =
      isolateActive ? QString::fromUtf8("Выйти из изоляции")
                    : QString::fromUtf8("Изолировать экземпляр");
  if (assemblyIsolationAction_) {
    assemblyIsolationAction_->setText(isolateText);
    assemblyIsolationAction_->setEnabled(isolateEnabled);
  }
  if (assemblyIsolationButton_) {
    assemblyIsolationButton_->setText(isolateText);
    assemblyIsolationButton_->setEnabled(isolateEnabled);
  }
  if (assemblyInsertButton_)
    assemblyInsertButton_->setEnabled(quiescent && !isolateActive);
  if (assemblyMateButton_)
    assemblyMateButton_->setEnabled(quiescent && !isolateActive);
  updateLinkedPartLifecycleAvailability();
}

void MainWindow::cancelAssemblyRename() noexcept {
  if (assemblyRenameCallbackRevision_ !=
      std::numeric_limits<std::uint64_t>::max())
    ++assemblyRenameCallbackRevision_;
  if (assemblyRenameDialog_) assemblyRenameDialog_->reject();
  assemblyRenameDialog_.clear();
  updateAssemblyRenameAvailability();
}

void MainWindow::beginAssemblyRename() {
  updateAssemblyRenameAvailability();
  if (!assemblyRenameAction_ || !assemblyRenameAction_->isEnabled() ||
      !viewport_ || !viewport_->assemblySession())
    return;
  const auto& selected = viewport_->selectedAssemblyOccurrenceView();
  if (!selected || selected->occurrence.path.depth() != 1) return;
  const auto resolved =
      productDocument_.resolveRootOccurrence(selected->occurrence.path);
  if (!resolved) return;

  RootOccurrenceAddress address{
      productDocument_.incarnation(), productDocument_.rootDefinitionId(),
      selected->occurrence.path, resolved.occurrence->targetDefinitionId()};
  const std::string before = resolved.occurrence->displayNameUtf8();
  const auto historyRevision = assemblyCommandHistory_.currentRevision();
  const std::uint64_t callbackRevision = assemblyRenameCallbackRevision_;
  QPointer<QInputDialog> dialog = new QInputDialog(this);
  dialog->setWindowTitle(QString::fromUtf8("Переименовать экземпляр"));
  dialog->setLabelText(QString::fromUtf8("Новое имя:"));
  dialog->setTextValue(QString::fromUtf8(
      before.data(), static_cast<qsizetype>(before.size())));
  dialog->setOkButtonText(QString::fromUtf8("Применить"));
  dialog->setCancelButtonText(QString::fromUtf8("Отмена"));
  dialog->setInputMode(QInputDialog::TextInput);
  assemblyRenameDialog_ = dialog;
  updateAssemblyRenameAvailability();
  QPointer<MainWindow> receiver(this);
  const int result = dialog->exec();
  if (!receiver || !dialog) return;
  const QString acceptedText = dialog->textValue();
  if (assemblyRenameDialog_ == dialog) assemblyRenameDialog_.clear();
  delete dialog.data();
  dialog.clear();
  if (result != QDialog::Accepted ||
      callbackRevision != assemblyRenameCallbackRevision_ ||
      historyRevision != assemblyCommandHistory_.currentRevision() ||
      assemblyInsertController_.active() || assemblyMateActive() ||
      partProjectActive() ||
      !viewport_ || !viewport_->assemblySession()) {
    updateAssemblyRenameAvailability();
    return;
  }

  const auto& currentSelection = viewport_->selectedAssemblyOccurrenceView();
  if (!currentSelection ||
      !currentSelection->isCurrentFor(
          viewport_->assemblySession()->publicationHandle()) ||
      currentSelection->occurrence.rootAssemblyDefinitionId !=
          address.rootDefinitionId ||
      currentSelection->occurrence.path != address.path) {
    updateAssemblyRenameAvailability();
    return;
  }
  const auto current = productDocument_.resolveRootOccurrence(address.path);
  if (!current || current.occurrence->targetDefinitionId() !=
                      address.expectedTargetDefinitionId ||
      current.occurrence->displayNameUtf8() != before) {
    updateAssemblyRenameAvailability();
    return;
  }

  const QByteArray utf8 = acceptedText.toUtf8();
  auto created = OccurrenceDelta::rename(
      std::move(address), before,
      std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
  if (!created) {
    statusBar()->showMessage(QString::fromUtf8("Недопустимое имя экземпляра"),
                             4000);
    updateAssemblyRenameAvailability();
    return;
  }
  auto semantic = captureAssemblyCommandSemanticState();
  auto afterState = occurrenceSemanticState(selected->occurrence.path);
  auto successorState = afterState ? copySemanticState(*afterState)
                                   : std::nullopt;
  if (!semantic || !afterState || !successorState) return;
  OccurrenceDeltaCommand command{std::move(*created.delta),
                                 std::move(*semantic),
                                 std::move(*afterState)};
  std::unique_ptr<PreparedAssemblyProjection> fullProjection;
  std::unique_ptr<PreparedAssemblyProjection> visibilityProjection;
  std::unique_ptr<PreparedAssemblyProjection> groundingProjection;
  std::unique_ptr<PreparedAssemblyProjection> suppressionProjection;
  std::unique_ptr<PreparedAssemblyProjection> duplicateProjection;
  std::unique_ptr<PreparedAssemblyProjection> removeProjection;
  std::unique_ptr<PreparedAssemblyProjection> definitionProjection;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex treeSelectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, fullProjection, visibilityProjection, groundingProjection,
      suppressionProjection, duplicateProjection, removeProjection,
      definitionProjection, treeOnly, treeSelectionIndex, preparedSelection,
      std::move(*successorState));
  const auto executed = assemblyCommandHistory_.executeOccurrenceDelta(
      productDocument_, std::move(command), prepare);
  if (executed.status == AssemblyCommandHistoryStatus::Committed) {
    installAssemblyTreeProjection(
        std::move(treeOnly), treeSelectionIndex,
        std::move(preparedSelection));
    setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
    updateUndoAvailability();
  }
  updateAssemblyRenameAvailability();
}

void MainWindow::toggleAssemblyOccurrenceVisibility() {
  updateAssemblyRenameAvailability();
  if (!assemblyVisibilityAction_ || !assemblyVisibilityAction_->isEnabled() ||
      !viewport_ || !viewport_->assemblySession())
    return;
  const auto& selected = viewport_->selectedAssemblyOccurrenceView();
  if (!selected || selected->occurrence.path.depth() != 1) return;
  const auto resolved =
      productDocument_.resolveRootOccurrence(selected->occurrence.path);
  if (!resolved) return;
  RootOccurrenceAddress address{
      productDocument_.incarnation(), productDocument_.rootDefinitionId(),
      selected->occurrence.path, resolved.occurrence->targetDefinitionId()};
  const bool beforeVisible = resolved.occurrence->flags().visible;
  auto semantic = captureAssemblyCommandSemanticState();
  auto afterState = occurrenceSemanticState(selected->occurrence.path);
  auto successorState = afterState ? copySemanticState(*afterState)
                                   : std::nullopt;
  if (!semantic || !afterState || !successorState) return;
  OccurrenceDeltaCommand command{
      OccurrenceDelta::visibility(std::move(address), beforeVisible,
                                  !beforeVisible),
      std::move(*semantic), std::move(*afterState)};
  std::unique_ptr<PreparedAssemblyProjection> fullProjection;
  std::unique_ptr<PreparedAssemblyProjection> visibilityProjection;
  std::unique_ptr<PreparedAssemblyProjection> groundingProjection;
  std::unique_ptr<PreparedAssemblyProjection> suppressionProjection;
  std::unique_ptr<PreparedAssemblyProjection> duplicateProjection;
  std::unique_ptr<PreparedAssemblyProjection> removeProjection;
  std::unique_ptr<PreparedAssemblyProjection> definitionProjection;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex selectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, fullProjection, visibilityProjection, groundingProjection,
      suppressionProjection, duplicateProjection, removeProjection,
      definitionProjection, treeOnly, selectionIndex,
      preparedSelection, std::move(*successorState));
  const auto executed = assemblyCommandHistory_.executeOccurrenceDelta(
      productDocument_, std::move(command), prepare);
  if (executed.status == AssemblyCommandHistoryStatus::Committed &&
      visibilityProjection) {
    installAssemblyVisibilityProjection(std::move(visibilityProjection),
                                        selectionIndex);
    setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
    updateUndoAvailability();
  }
  updateAssemblyRenameAvailability();
}

void MainWindow::toggleAssemblyOccurrenceGrounded() {
  updateAssemblyRenameAvailability();
  if (!assemblyGroundingAction_ || !assemblyGroundingAction_->isEnabled() ||
      !viewport_ || !viewport_->assemblySession())
    return;
  const auto& selected = viewport_->selectedAssemblyOccurrenceView();
  if (!selected ||
      !selected->isCurrentFor(
          viewport_->assemblySession()->publicationHandle()) ||
      selected->occurrence.rootAssemblyDefinitionId !=
          productDocument_.rootDefinitionId() ||
      selected->occurrence.path.depth() != 1)
    return;
  const auto resolved =
      productDocument_.resolveRootOccurrence(selected->occurrence.path);
  if (!resolved) return;
  RootOccurrenceAddress address{
      productDocument_.incarnation(), productDocument_.rootDefinitionId(),
      selected->occurrence.path, resolved.occurrence->targetDefinitionId()};
  const bool beforeGrounded = resolved.occurrence->flags().grounded;
  auto semantic = captureAssemblyCommandSemanticState();
  auto afterState = occurrenceSemanticState(selected->occurrence.path);
  auto successorState = afterState ? copySemanticState(*afterState)
                                   : std::nullopt;
  if (!semantic || !afterState || !successorState) return;
  OccurrenceDeltaCommand command{
      OccurrenceDelta::grounded(std::move(address), beforeGrounded,
                                !beforeGrounded),
      std::move(*semantic), std::move(*afterState)};
  std::unique_ptr<PreparedAssemblyProjection> fullProjection;
  std::unique_ptr<PreparedAssemblyProjection> visibilityProjection;
  std::unique_ptr<PreparedAssemblyProjection> groundingProjection;
  std::unique_ptr<PreparedAssemblyProjection> suppressionProjection;
  std::unique_ptr<PreparedAssemblyProjection> duplicateProjection;
  std::unique_ptr<PreparedAssemblyProjection> removeProjection;
  std::unique_ptr<PreparedAssemblyProjection> definitionProjection;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex selectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, fullProjection, visibilityProjection, groundingProjection,
      suppressionProjection, duplicateProjection, removeProjection,
      definitionProjection, treeOnly, selectionIndex,
      preparedSelection, std::move(*successorState));
  const auto executed = assemblyCommandHistory_.executeOccurrenceDelta(
      productDocument_, std::move(command), prepare);
  if (executed.status == AssemblyCommandHistoryStatus::Committed &&
      groundingProjection) {
    installAssemblyGroundingProjection(std::move(groundingProjection),
                                       selectionIndex);
    setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
    updateUndoAvailability();
  }
  updateAssemblyRenameAvailability();
}

void MainWindow::toggleAssemblyOccurrenceSuppressed() {
  updateAssemblyRenameAvailability();
  if (!assemblySuppressionAction_ ||
      !assemblySuppressionAction_->isEnabled() || !viewport_ ||
      !viewport_->assemblySession())
    return;
  const auto& selected = viewport_->selectedAssemblyOccurrenceView();
  if (!selected ||
      !selected->isCurrentFor(
          viewport_->assemblySession()->publicationHandle()) ||
      selected->occurrence.rootAssemblyDefinitionId !=
          productDocument_.rootDefinitionId() ||
      selected->occurrence.path.depth() != 1)
    return;
  const auto resolved =
      productDocument_.resolveRootOccurrence(selected->occurrence.path);
  if (!resolved) return;
  RootOccurrenceAddress address{
      productDocument_.incarnation(), productDocument_.rootDefinitionId(),
      selected->occurrence.path, resolved.occurrence->targetDefinitionId()};
  const bool beforeSuppressed = resolved.occurrence->flags().suppressed;
  auto semantic = captureAssemblyCommandSemanticState();
  auto afterState = occurrenceSemanticState(selected->occurrence.path);
  auto successorState = afterState ? copySemanticState(*afterState)
                                   : std::nullopt;
  if (!semantic || !afterState || !successorState) return;
  OccurrenceDeltaCommand command{
      OccurrenceDelta::suppressed(std::move(address), beforeSuppressed,
                                  !beforeSuppressed),
      std::move(*semantic), std::move(*afterState)};
  std::unique_ptr<PreparedAssemblyProjection> fullProjection;
  std::unique_ptr<PreparedAssemblyProjection> visibilityProjection;
  std::unique_ptr<PreparedAssemblyProjection> groundingProjection;
  std::unique_ptr<PreparedAssemblyProjection> suppressionProjection;
  std::unique_ptr<PreparedAssemblyProjection> duplicateProjection;
  std::unique_ptr<PreparedAssemblyProjection> removeProjection;
  std::unique_ptr<PreparedAssemblyProjection> definitionProjection;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex selectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, fullProjection, visibilityProjection, groundingProjection,
      suppressionProjection, duplicateProjection, removeProjection,
      definitionProjection, treeOnly, selectionIndex,
      preparedSelection, std::move(*successorState));
  const auto executed = assemblyCommandHistory_.executeOccurrenceDelta(
      productDocument_, std::move(command), prepare);
  if (executed.status == AssemblyCommandHistoryStatus::Committed &&
      suppressionProjection) {
    installAssemblySuppressionProjection(std::move(suppressionProjection),
                                         selectionIndex);
    setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
    updateUndoAvailability();
  }
  updateAssemblyRenameAvailability();
}

void MainWindow::duplicateAssemblyOccurrence() {
  updateAssemblyRenameAvailability();
  if (!assemblyDuplicateAction_ || !assemblyDuplicateAction_->isEnabled() ||
      !viewport_ || !viewport_->assemblySession())
    return;
  const auto& selected = viewport_->selectedAssemblyOccurrenceView();
  if (!selected ||
      !selected->isCurrentFor(
          viewport_->assemblySession()->publicationHandle()) ||
      selected->occurrence.rootAssemblyDefinitionId !=
          productDocument_.rootDefinitionId() ||
      selected->occurrence.path.depth() != 1)
    return;
  const auto resolved =
      productDocument_.resolveRootOccurrence(selected->occurrence.path);
  const auto duplicateId = productDocument_.lowestFreeRootOccurrenceId();
  if (!resolved || !duplicateId ||
      !productDocument_.definitions().findPart(
          resolved.occurrence->targetDefinitionId()))
    return;

  try {
    const auto duplicatePath = OccurrencePath::create({*duplicateId});
    if (!duplicatePath) return;
    DuplicateOccurrenceColdPayload payload{
        {productDocument_.incarnation(), productDocument_.rootDefinitionId(),
         selected->occurrence.path,
         resolved.occurrence->targetDefinitionId()},
        *duplicateId,
        resolved.occurrence->targetDefinitionId(),
        resolved.occurrence->placement(),
        resolved.occurrence->homePlacement(),
        resolved.occurrence->flags(),
        resolved.occurrence->displayNameUtf8()};
    auto semantic = captureAssemblyCommandSemanticState();
    auto afterState = occurrenceSemanticState(*duplicatePath.path);
    auto successorState = afterState ? copySemanticState(*afterState)
                                     : std::nullopt;
    if (!semantic || !afterState || !successorState) return;
    DuplicateOccurrenceColdCommand command{
        std::move(payload), std::move(*semantic), std::move(*afterState)};
    std::unique_ptr<PreparedAssemblyProjection> fullProjection;
    std::unique_ptr<PreparedAssemblyProjection> visibilityProjection;
    std::unique_ptr<PreparedAssemblyProjection> groundingProjection;
    std::unique_ptr<PreparedAssemblyProjection> suppressionProjection;
    std::unique_ptr<PreparedAssemblyProjection> duplicateProjection;
    std::unique_ptr<PreparedAssemblyProjection> removeProjection;
    std::unique_ptr<PreparedAssemblyProjection> definitionProjection;
    std::unique_ptr<AssemblyTreeModel> treeOnly;
    QModelIndex selectionIndex;
    std::optional<PreparedAssemblySelectionState> preparedSelection;
    AssemblyProjectionSuccessor prepare(
        *this, fullProjection, visibilityProjection, groundingProjection,
        suppressionProjection, duplicateProjection, removeProjection,
        definitionProjection, treeOnly, selectionIndex,
        preparedSelection, std::move(*successorState));
    const auto executed = assemblyCommandHistory_.executeDuplicate(
        productDocument_, std::move(command), prepare);
    if (executed.status == AssemblyCommandHistoryStatus::Committed &&
        duplicateProjection) {
      installAssemblyDuplicateProjection(std::move(duplicateProjection),
                                         selectionIndex);
      setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
      updateUndoAvailability();
    }
  } catch (...) {
    // Command construction is still pre-mutation; allocation failure is an
    // exact no-op and leaves the current publication/selection untouched.
  }
  updateAssemblyRenameAvailability();
}

void MainWindow::removeAssemblyOccurrence() {
  updateAssemblyRenameAvailability();
  if (!assemblyRemoveAction_ || !assemblyRemoveAction_->isEnabled() ||
      !viewport_ || !viewport_->assemblySession())
    return;
  const auto& selected = viewport_->selectedAssemblyOccurrenceView();
  if (!selected ||
      !selected->isCurrentFor(
          viewport_->assemblySession()->publicationHandle()) ||
      selected->occurrence.rootAssemblyDefinitionId !=
          productDocument_.rootDefinitionId() ||
      selected->occurrence.path.depth() != 1)
    return;
  const auto resolved =
      productDocument_.resolveRootOccurrence(selected->occurrence.path);
  if (!resolved ||
      !productDocument_.definitions().findPart(
          resolved.occurrence->targetDefinitionId()))
    return;

  try {
    RemoveOccurrenceColdPayload payload{
        {productDocument_.incarnation(), productDocument_.rootDefinitionId(),
         selected->occurrence.path,
         resolved.occurrence->targetDefinitionId()},
        resolved.occurrence->placement(), resolved.occurrence->flags(),
        resolved.occurrence->displayNameUtf8(),
        resolved.occurrence->homePlacement()};
    auto semantic = captureAssemblyCommandSemanticState();
    if (!semantic) return;
    RemoveOccurrenceColdCommand command{
        std::move(payload), std::move(*semantic), {}};
    std::unique_ptr<PreparedAssemblyProjection> fullProjection;
    std::unique_ptr<PreparedAssemblyProjection> visibilityProjection;
    std::unique_ptr<PreparedAssemblyProjection> groundingProjection;
    std::unique_ptr<PreparedAssemblyProjection> suppressionProjection;
    std::unique_ptr<PreparedAssemblyProjection> duplicateProjection;
    std::unique_ptr<PreparedAssemblyProjection> removeProjection;
    std::unique_ptr<PreparedAssemblyProjection> definitionProjection;
    std::unique_ptr<AssemblyTreeModel> treeOnly;
    QModelIndex selectionIndex;
    std::optional<PreparedAssemblySelectionState> preparedSelection;
    AssemblyProjectionSuccessor prepare(
        *this, fullProjection, visibilityProjection, groundingProjection,
        suppressionProjection, duplicateProjection, removeProjection,
        definitionProjection, treeOnly, selectionIndex, preparedSelection,
        AssemblyCommandSemanticState{});
    const auto executed = assemblyCommandHistory_.executeRemove(
        productDocument_, std::move(command), prepare);
    if (executed.status == AssemblyCommandHistoryStatus::Committed &&
        removeProjection) {
      installAssemblyRemoveProjection(std::move(removeProjection),
                                      selectionIndex);
      setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
      updateUndoAvailability();
    }
  } catch (...) {
    // Cold command construction happens before mutation; allocation failure
    // therefore preserves the exact product/publication/history state.
  }
  updateAssemblyRenameAvailability();
}

void MainWindow::toggleAssemblyIsolation() {
  updateAssemblyRenameAvailability();
  if (!assemblyIsolationAction_ || !assemblyIsolationAction_->isEnabled() ||
      !viewport_ || !viewport_->assemblySession())
    return;
  std::optional<AssemblyOccurrenceIdentity> target;
  if (!viewport_->assemblySession()->isolationActive()) {
    const auto& selected = viewport_->selectedAssemblyOccurrenceView();
    if (!selected) return;
    try {
      target = selected->occurrence;
    } catch (...) {
      return;
    }
  }
  std::optional<PreparedAssemblyIsolationState> prepared;
  std::unique_ptr<AssemblyTreeModel> tree;
  QModelIndex selectionIndex;
  if (!prepareAssemblyIsolationProjection(target, prepared, tree,
                                           selectionIndex))
    return;
  installAssemblyIsolationProjection(std::move(prepared), std::move(tree),
                                     selectionIndex);
  updateAssemblyRenameAvailability();
}

void MainWindow::beginAssemblyInsert() {
  if (partProjectActive() || assemblyMateActive()) return;
  if (viewport_ && viewport_->assemblySession() &&
      viewport_->assemblySession()->isolationActive())
    return;
  const auto* root = productDocument_.rootAssemblyDefinition();
  if (!root) return;
  if (!assemblyInsertController_.active()) {
    std::uint64_t definitionValue = 1;
    while (productDocument_.definitions().contains(
        DefinitionId{definitionValue})) {
      if (definitionValue == std::numeric_limits<std::uint64_t>::max()) return;
      ++definitionValue;
    }
    std::uint64_t occurrenceValue = 1;
    while (root->find(OccurrenceId{occurrenceValue})) {
      if (occurrenceValue == std::numeric_limits<std::uint64_t>::max()) return;
      ++occurrenceValue;
    }
    assemblyInsertController_.activate(
        assemblyInsertSession_, DefinitionId{definitionValue},
        OccurrenceId{occurrenceValue});
    resetAssemblyPlacementControls(false);
    updateAssemblyRenameAvailability();
  } else if (assemblyInsertSession_.state() !=
                 AssemblyInsertToolState::AwaitingSource &&
             assemblyInsertSession_.state() !=
                 AssemblyInsertToolState::Placing) {
    return;
  }
  if (assemblyCancelButton_) assemblyCancelButton_->setEnabled(true);
  static_cast<void>(assemblyInsertSession_.beginNestedSelection());
  const std::uint64_t revision = assemblyInsertController_.revision();
  const std::uint64_t callbackRevision = assemblyInsertCallbackRevision_;
  QPointer<MainWindow> receiver(this);
  const QString path = QFileDialog::getOpenFileName(
      this, QString::fromUtf8("Вставить компонент"), QString{},
      QStringLiteral("SolidarCAD (*.solidar)"));
  if (!receiver || assemblyInsertController_.revision() != revision ||
      assemblyInsertCallbackRevision_ != callbackRevision)
    return;
  static_cast<void>(assemblyInsertSession_.finishNested());
  if (path.isEmpty()) return;
  auto staged = projectApplicationService_.stageEmbeddedPart(path);
  if (!staged.succeeded()) {
    statusBar()->showMessage(
        QString::fromUtf8("Компонент не подготовлен: ") + staged.status.detail,
        4000);
    return;
  }
  if (!viewport_->prepareAssemblyPlacementPreview(*staged.staged->document)) {
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось построить предпросмотр компонента"),
        4000);
    return;
  }
  if (!assemblyInsertSession_.acceptSource(std::move(*staged.staged))) {
    return;
  }
  resetAssemblyPlacementControls(true);
  static_cast<void>(viewport_->setAssemblyPlacementPreviewTransform(
      RigidTransform3d::identity()));
  if (assemblyApplyButton_) assemblyApplyButton_->setEnabled(true);
  if (assemblyCancelButton_) assemblyCancelButton_->setEnabled(true);
  for (auto* spin : assemblyTranslationSpins_) spin->setEnabled(true);
  for (auto* spin : assemblyRotationSpins_) spin->setEnabled(true);
  statusBar()->showMessage(
      QString::fromUtf8("Компонент подготовлен: задайте размещение"));
}

void MainWindow::beginAssemblyMate() {
  if (partProjectActive() || assemblyInsertController_.active() ||
      assemblyCommandHistory_.isBusy() || definitionEditContext_ ||
      !assemblyRenameDialog_.isNull() || !viewport_ ||
      !viewport_->assemblySession() ||
      viewport_->assemblySession()->isolationActive())
    return;
  auto& session = assemblyMateController_->session();
  if (!session.active() &&
      !session.begin(productDocument_,
                     viewport_->assemblySession()->publicationHandle()))
    return;
  viewport_->setAssemblyMatePickMode(true);
  updateAssemblyMateUi();
  updateAssemblyRenameAvailability();
  statusBar()->showMessage(
      QString::fromUtf8("Сопряжение: выберите первую плоскую грань"));
}

void MainWindow::acceptAssemblyMatePick(const AssemblyFaceRayHit& hit) {
  if (!assemblyMateActive() || !viewport_ || !viewport_->assemblySession())
    return;
  QPointer<MainWindow> receiver(this);
  const auto incarnation = productDocument_.incarnation();
  const auto root = productDocument_.rootDefinitionId();
  const auto publication = viewport_->assemblySession()->publicationHandle();
  const auto result = assemblyMateController_->session().acceptPick(
      productDocument_, hit, publication);
  if (assemblyMateReentryHookForTesting_) {
    auto hook = std::move(assemblyMateReentryHookForTesting_);
    assemblyMateReentryHookForTesting_ = {};
    try {
      hook();
    } catch (...) {
      // Test-only observer failures must obey the same Qt event firewall.
    }
  }
  if (!receiver || !assemblyMateActive() || !viewport_ ||
      !viewport_->assemblySession() ||
      productDocument_.incarnation() != incarnation ||
      productDocument_.rootDefinitionId() != root ||
      !viewport_->assemblySession()->publicationHandle().isSameCurrentPublication(
          publication))
    return;
  const auto revision = assemblyMateController_->session().callbackRevision();
  if (assemblyMateController_->session().currentPreview())
    static_cast<void>(presentCurrentAssemblyMatePreview(revision));
  updateAssemblyMateUi();
  if (result.status != AssemblyMateToolPickStatus::Accepted) {
    statusBar()->showMessage(
        QString::fromUtf8("Грань не подходит для сопряжения"), 3500);
  } else if (assemblyMateController_->session().state() ==
             AssemblyMateToolState::SelectingSecond) {
    statusBar()->showMessage(
        QString::fromUtf8("Сопряжение: выберите вторую плоскую грань"));
  } else if (!assemblyMateController_->session().canApply()) {
    statusBar()->showMessage(
        QString::fromUtf8("Сопряжение не может быть рассчитано"), 3500);
  }
}

bool MainWindow::presentCurrentAssemblyMatePreview(
    std::uint64_t expectedRevision) noexcept {
  if (!assemblyMateActive() || !viewport_ || !viewport_->assemblySession())
    return false;
  auto& session = assemblyMateController_->session();
  if (session.callbackRevision() != expectedRevision ||
      !session.currentPreview())
    return false;
  if (assemblyMatePreviewPrepareFailureForTesting_) {
    assemblyMatePreviewPrepareFailureForTesting_ = false;
    session.presentationFailed(expectedRevision);
    return false;
  }
  auto scene = viewport_->assemblySession()->prepareMatePreview(
      session.currentPreview()->candidate());
  const bool rejectInstall =
      std::exchange(assemblyMatePreviewInstallFailureForTesting_, false);
  if (!scene || rejectInstall || session.callbackRevision() != expectedRevision ||
      !viewport_->installAssemblyMatePreview(std::move(scene))) {
    session.presentationFailed(expectedRevision);
    return false;
  }
  return session.presentationSucceeded(expectedRevision);
}

void MainWindow::updateAssemblyMateUi() noexcept {
  const auto& session = assemblyMateController_->session();
  const bool active = session.active();
  if (assemblyMateButton_) assemblyMateButton_->setEnabled(!active);
  if (assemblyMateOpposedCheck_) {
    const QSignalBlocker blocker(assemblyMateOpposedCheck_);
    assemblyMateOpposedCheck_->setVisible(active);
    assemblyMateOpposedCheck_->setEnabled(
        active && session.state() != AssemblyMateToolState::CommitPending);
    assemblyMateOpposedCheck_->setChecked(
        session.orientation() == PlanarMateOrientation::Opposed);
  }
  if (assemblyMateStatusLabel_) {
    assemblyMateStatusLabel_->setVisible(active);
    const auto text = session.state() == AssemblyMateToolState::SelectingFirst
                          ? QString::fromUtf8("Первая грань")
                      : session.state() ==
                                AssemblyMateToolState::SelectingSecond
                          ? QString::fromUtf8("Вторая грань")
                      : session.canApply()
                          ? QString::fromUtf8("Предпросмотр готов")
                          : QString::fromUtf8("Нет решения");
    assemblyMateStatusLabel_->setText(text);
  }
  if (assemblyApplyButton_)
    assemblyApplyButton_->setEnabled(
        active ? session.canApply() : assemblyInsertController_.active());
  if (assemblyCancelButton_)
    assemblyCancelButton_->setEnabled(active ||
                                      assemblyInsertController_.active());
}

void MainWindow::applyAssemblyMate() {
  if (!assemblyMateActive()) return;
  auto fresh = assemblyMateController_->session().prepareCommit(
      productDocument_);
  if (!fresh) {
    updateAssemblyMateUi();
    return;
  }
  const bool needsPromotion =
      projectPersistenceState_.format == project::ProjectPersistenceFormat::V7 ||
      projectPersistenceState_.format == project::ProjectPersistenceFormat::V8;
  auto committedPersistenceState = projectPersistenceState_;
  if (needsPromotion &&
      !projectApplicationService_.promoteAssemblyPersistenceToV9(
           committedPersistenceState, productDocument_).succeeded()) {
    assemblyMateController_->session().commitFailed();
    updateAssemblyMateUi();
    return;
  }
  auto beforeState = captureAssemblyCommandSemanticState();
  auto afterState = beforeState ? copySemanticState(*beforeState)
                                : std::nullopt;
  auto successorState = afterState ? copySemanticState(*afterState)
                                   : std::nullopt;
  if (!beforeState || !afterState || !successorState) {
    assemblyMateController_->session().commitFailed();
    updateAssemblyMateUi();
    return;
  }
  std::unique_ptr<PreparedAssemblyProjection> successor;
  std::unique_ptr<PreparedAssemblyProjection> visibilitySuccessor;
  std::unique_ptr<PreparedAssemblyProjection> groundingSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> suppressionSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> duplicateSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> removeSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> definitionSuccessor;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex treeSelectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, successor, visibilitySuccessor, groundingSuccessor,
      suppressionSuccessor, duplicateSuccessor, removeSuccessor,
      definitionSuccessor, treeOnly, treeSelectionIndex, preparedSelection,
      std::move(*successorState));
  PlanarMateColdCommand command{
      std::move(fresh),
      std::move(*beforeState), std::move(*afterState)};
  const auto result = assemblyCommandHistory_.executePlanarMate(
      productDocument_, std::move(command), prepare);
  if (result.status != AssemblyCommandHistoryStatus::Committed) {
    assemblyMateController_->session().commitFailed();
    updateAssemblyMateUi();
    return;
  }
  if (successor) installAssemblyProjection(std::move(successor));
  if (needsPromotion)
    projectPersistenceState_ = std::move(committedPersistenceState);
  mateRuntimeDiagnostics_.clear();
  mateDiagnosticsValidityStamp_ = captureMateDiagnosticsValidityStamp();
  cancelAssemblyMate();
  setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
  updateUndoAvailability();
}

void MainWindow::cancelAssemblyMate() noexcept {
  if (assemblyMateController_) assemblyMateController_->cancel();
  if (viewport_) {
    viewport_->clearAssemblyMatePreview();
    viewport_->setAssemblyMatePickMode(false);
  }
  updateAssemblyMateUi();
  updateAssemblyRenameAvailability();
}

void MainWindow::updateAssemblyInsertPlacement() noexcept {
  if (assemblyInsertSession_.state() != AssemblyInsertToolState::Placing)
    return;
  std::array<double, 3> translation{};
  std::array<double, 3> rotation{};
  for (std::size_t i = 0; i < 3; ++i) {
    translation[i] = assemblyTranslationSpins_[i]->value();
    rotation[i] = assemblyRotationSpins_[i]->value();
  }
  static_cast<void>(assemblyInsertSession_.updateTranslation(translation));
  static_cast<void>(assemblyInsertSession_.updateRotationDegrees(rotation));
  static_cast<void>(viewport_->setAssemblyPlacementPreviewTransform(
      assemblyInsertSession_.placement()));
}

void MainWindow::cancelAssemblyInsert() noexcept {
  cancelAssemblyInsertBeforeReplacement();
  detachAssemblyInsertUi(false);
}

void MainWindow::resetAssemblyPlacementControls(bool enabled) noexcept {
  for (auto* spin : assemblyTranslationSpins_) {
    if (!spin) continue;
    const QSignalBlocker blocker(spin);
    spin->setValue(0.0);
    spin->setEnabled(enabled);
  }
  for (auto* spin : assemblyRotationSpins_) {
    if (!spin) continue;
    const QSignalBlocker blocker(spin);
    spin->setValue(0.0);
    spin->setEnabled(enabled);
  }
}

void MainWindow::traceAssemblyReplacementStep(
    AssemblyInsertTeardownStep step, bool sourceAlive) noexcept {
  if (!assemblyInsertTeardownObserver_) return;
  try {
    assemblyInsertTeardownObserver_(step, sourceAlive);
  } catch (...) {
  }
}

void MainWindow::cancelAssemblyInsertBeforeReplacement() noexcept {
  cancelAssemblyMate();
  cancelAssemblyRename();
  if (assemblyInsertCallbackRevision_ !=
      std::numeric_limits<std::uint64_t>::max())
    ++assemblyInsertCallbackRevision_;
  invalidatePreviewUpdates();
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::InsertCallbacksInvalidated, false);
  bool sourceWasAlive = assemblyInsertSession_.source() != nullptr &&
                        assemblyInsertSession_.source()->document != nullptr;
  const auto beforeRelease = [&sourceWasAlive](
                                 const StagedEmbeddedPart* source) noexcept {
    sourceWasAlive = sourceWasAlive ||
                     (source != nullptr && source->document != nullptr);
  };
  assemblyInsertController_.cancelActive(
      beforeRelease, [this, &sourceWasAlive] {
        traceAssemblyReplacementStep(
            AssemblyInsertTeardownStep::InsertControllerCancelled,
            sourceWasAlive);
      });
  assemblyInsertSession_.cancel(beforeRelease);
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::InsertSessionCancelled, sourceWasAlive);
}

void MainWindow::preparePartDesignForReplacement() noexcept {
  const bool oldProductAlive = productDocument_.definitions().size() != 0;
  static_cast<void>(partDesignCoordinator_.prepareDocumentReplacement(
      [this, oldProductAlive](PartDesignReplacementStep step) {
        traceAssemblyReplacementStep(
            step == PartDesignReplacementStep::ControllerCancelled
                ? AssemblyInsertTeardownStep::PartDesignPrepared
                : AssemblyInsertTeardownStep::PartDesignSessionsCancelled,
            oldProductAlive);
      }));
}

void MainWindow::detachDefinitionEditForReplacement() noexcept {
  componentModelHistories_.clear();
  suspendedComponentModelHistory_.reset();
  if (!definitionEditContext_ && !componentDefinitionEditContext_) return;
  invalidatePreviewUpdates();
  if (viewport_) viewport_->resetScene();
  suspendedAssemblySession_.reset();
  suspendedComponentSession_.reset();
  componentEditDocument_.reset();
  definitionEditContext_.reset();
  componentDefinitionEditContext_.reset();
  modelHistory_.clear(true);
  if (definitionEditBreadcrumb_) definitionEditBreadcrumb_->hide();
  if (definitionEditBackButton_) definitionEditBackButton_->hide();
}

void MainWindow::detachAssemblyInsertUi(bool hardReplacement) noexcept {
  if (viewport_) viewport_->clearAssemblyPlacementPreview();
  if (hardReplacement && viewport_) {
    resetTransientModelingPresentation();
    static_cast<void>(
        viewport_->setSelectedAssemblyOccurrence(std::nullopt, false));
    if (assemblyTreeView_ && assemblyTreeView_->selectionModel()) {
      const QSignalBlocker blocker(assemblyTreeView_->selectionModel());
      assemblyTreeView_->clearSelection();
      assemblyTreeView_->setCurrentIndex({});
    }
  }
  resetAssemblyPlacementControls(false);
  if (assemblyApplyButton_) assemblyApplyButton_->setEnabled(false);
  if (assemblyCancelButton_) assemblyCancelButton_->setEnabled(false);
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::GhostUiDetached, false);
  updateAssemblyRenameAvailability();
}

void MainWindow::clearAssemblyCommandHistoryForReplacement() noexcept {
  assemblyCommandHistory_.clear();
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::AssemblyHistoryCleared, false);
}

std::optional<MainWindow::MateDiagnosticsValidityStamp>
MainWindow::captureMateDiagnosticsValidityStamp() const noexcept {
  try {
    MateDiagnosticsValidityStamp stamp;
    stamp.historyRevision = assemblyCommandHistory_.currentRevision();
    stamp.historyCommandCount = assemblyCommandHistory_.size();
    stamp.historyCursor = assemblyCommandHistory_.cursor();
    for (const DefinitionId id :
         productDocument_.definitions().definitionIds()) {
      const auto* assembly = productDocument_.definitions().findAssembly(id);
      if (!assembly) continue;
      const auto& registry = assembly->mateRegistry();
      stamp.registries.push_back({id, registry.revision(),
                                  registry.fingerprint(), registry.options()});
    }
    return stamp;
  } catch (...) {
    return std::nullopt;
  }
}

MainWindow::MateDiagnosticsValidity
MainWindow::mateDiagnosticsValidity() const noexcept {
  if (projectKind_ != EditorProjectKind::AssemblyReadOnly ||
      projectPersistenceState_.format !=
          project::ProjectPersistenceFormat::V9 ||
      !mateDiagnosticsValidityStamp_)
    return MateDiagnosticsValidity::Unchecked;
  const auto current = captureMateDiagnosticsValidityStamp();
  if (!current ||
      current->historyRevision !=
          mateDiagnosticsValidityStamp_->historyRevision ||
      current->historyCommandCount !=
          mateDiagnosticsValidityStamp_->historyCommandCount ||
      current->historyCursor != mateDiagnosticsValidityStamp_->historyCursor ||
      current->registries != mateDiagnosticsValidityStamp_->registries)
    return MateDiagnosticsValidity::Unchecked;
  return MateDiagnosticsValidity::Current;
}

std::optional<AssemblyCommandSemanticState>
MainWindow::captureAssemblyCommandSemanticState() const noexcept {
  try {
    AssemblyCommandSemanticState state;
    std::optional<std::reference_wrapper<const AssemblyTreeNodeAddress>>
        address;
    if (assemblyTreeModel_ && assemblyTreeView_)
      address = assemblyTreeModel_->addressForIndex(
          assemblyTreeView_->currentIndex());
    if (address &&
        (address->get().kind == AssemblyTreeNodeKind::PartDefinition ||
         address->get().kind == AssemblyTreeNodeKind::AssemblyDefinition ||
         address->get().kind ==
             AssemblyTreeNodeKind::RootAssemblyDefinition)) {
      state.selectedDefinition = address->get().definitionId;
      return state;
    }
    if (!viewport_ || !viewport_->assemblySession()) return state;
    const auto& selected = viewport_->selectedAssemblyOccurrenceView();
    if (!selected ||
        !selected->isCurrentFor(
            viewport_->assemblySession()->publicationHandle()) ||
        selected->occurrence.rootAssemblyDefinitionId !=
            productDocument_.rootDefinitionId())
      return state;
    state.selectedOccurrence = selected->occurrence.path;
    return state;
  } catch (...) {
    return std::nullopt;
  }
}

void MainWindow::applyAssemblyInsert() {
  if (partProjectActive() || !assemblyInsertSession_.beginCommit()) return;
  const OccurrenceId inserted = assemblyInsertSession_.proposedOccurrenceId();
  const auto createdInsertedPath = OccurrencePath::create({inserted});
  if (!createdInsertedPath) {
    assemblyInsertSession_.restoreAfterFailedCommit();
    return;
  }
  const OccurrencePath insertedPath = *createdInsertedPath.path;
  auto beforeState = captureAssemblyCommandSemanticState();
  auto afterState = occurrenceSemanticState(insertedPath);
  auto successorState = afterState ? copySemanticState(*afterState)
                                   : std::nullopt;
  if (!beforeState || !afterState || !successorState) {
    assemblyInsertSession_.restoreAfterFailedCommit();
    return;
  }
  try {
    EmbeddedPartInsertColdCommand command{
        assemblyInsertSession_.proposedDefinitionId(),
        inserted,
        assemblyInsertSession_.placement(), {},
        coldBytes(assemblyInsertSession_.authoredDocument()),
        {},
        std::move(*beforeState),
        std::move(*afterState)};
    if (assemblyCommandHistory_.prepareRecord(std::move(command)) !=
        AssemblyCommandHistoryStatus::Committed) {
      assemblyInsertSession_.restoreAfterFailedCommit();
      return;
    }
  } catch (...) {
    assemblyInsertSession_.restoreAfterFailedCommit();
    return;
  }
  auto payload = assemblyInsertSession_.takeRuntimePayload();
  std::unique_ptr<PreparedAssemblyProjection> successor;
  std::unique_ptr<PreparedAssemblyProjection> visibilitySuccessor;
  std::unique_ptr<PreparedAssemblyProjection> groundingSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> suppressionSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> duplicateSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> removeSuccessor;
  std::unique_ptr<PreparedAssemblyProjection> definitionSuccessor;
  std::unique_ptr<AssemblyTreeModel> treeOnly;
  QModelIndex treeSelectionIndex;
  std::optional<PreparedAssemblySelectionState> preparedSelection;
  AssemblyProjectionSuccessor prepare(
      *this, successor, visibilitySuccessor, groundingSuccessor,
      suppressionSuccessor, duplicateSuccessor, removeSuccessor,
      definitionSuccessor, treeOnly, treeSelectionIndex, preparedSelection,
      std::move(*successorState));
  const auto status =
      productDocument_.applyEmbeddedPartInsert(payload, prepare);
  if (status != EmbeddedPartInsertStatus::Committed) {
    assemblyCommandHistory_.discardPrepared();
    assemblyInsertSession_.restoreRuntimePayload(std::move(payload));
    assemblyInsertSession_.restoreAfterFailedCommit();
    return;
  }
  if (assemblyCommandHistory_.commitPrepared() !=
      AssemblyCommandHistoryStatus::Committed)
    return;
  installAssemblyProjection(std::move(successor));
  assemblyInsertSession_.finishCommit();
  cancelAssemblyInsert();
  setWindowModified(!assemblyCommandHistory_.isAtSavepoint());
  updateUndoAvailability();
}

void MainWindow::applyProjectKind(EditorProjectKind kind) noexcept {
  projectKind_ = kind;
  const bool part = partModelingContextActive();
  const bool standalone = standalonePartProjectActive();
  const bool component = componentProductActive();
  const bool modelWorkspace = modelWorkspaceActive();
  const bool writableAssembly =
      assemblyProductActive() &&
      isWritableAssemblyFormat(projectPersistenceState_.format);
  const bool writableComponent =
      component && isWritableComponentFormat(projectPersistenceState_.format);
  // Embedded definition documents are authoritative ProductDocument state.
  // Until an individual Part command is ported to the detached-candidate
  // boundary, it must be visible for context but disabled fail-closed.
  const bool definitionMoveAuthoring = definitionEditContext_.has_value();
  const bool componentAuthoring = componentDefinitionEditContext_.has_value();
  const bool rootComponentAuthoring =
      componentAuthoring &&
      componentDefinitionEditContext_->targetDefinitionId ==
          componentDefinitionEditContext_->rootDefinitionId &&
      !componentDefinitionEditContext_->occurrencePath;
  const bool partAuthoring = standalone || componentAuthoring;
  if (modelRibbon_) modelRibbon_->setEnabled(modelWorkspace);
  if (modelRibbon_) {
    if (definitionMoveAuthoring) {
      for (auto* button : modelRibbon_->findChildren<QToolButton*>())
        button->setEnabled(button->objectName() ==
                           QStringLiteral("moveCommand"));
      modelRibbon_->setAuthoringEnabled(false, true);
      modelRibbon_->setMeshQualityControlEnabled(false);
    } else {
      for (auto* button : modelRibbon_->findChildren<QToolButton*>())
        button->setEnabled(modelWorkspace);
      modelRibbon_->setAuthoringEnabled(partAuthoring);
      modelRibbon_->setMeshQualityControlEnabled(modelWorkspace && !component);
      if (component) {
        if (auto* ruler = modelRibbon_->findChild<QToolButton*>(
                QStringLiteral("rulerCommand")))
          ruler->setEnabled(false);
      }
    }
    modelRibbon_->setComponentEditing(componentAuthoring,
                                      rootComponentAuthoring);
  }
  if (sketchRibbon_) sketchRibbon_->setEnabled(partAuthoring);
  if (sketchCanvas_) sketchCanvas_->setEnabled(partAuthoring);
  if (featureTree_) featureTree_->setEnabled(partAuthoring);
  if (historyTimeline_) historyTimeline_->setEnabled(partAuthoring);
  if (toolParametersDock_) toolParametersDock_->setEnabled(partAuthoring);
  if (revolveDock_) revolveDock_->setEnabled(partAuthoring);
  if (mirrorDock_) mirrorDock_->setEnabled(partAuthoring);
  if (moveDock_)
    moveDock_->setEnabled(standalone || componentAuthoring ||
                          definitionMoveAuthoring);
  if (linearPatternDock_) linearPatternDock_->setEnabled(partAuthoring);
  if (circularPatternDock_) circularPatternDock_->setEnabled(partAuthoring);
  if (extrusionDock_) extrusionDock_->setEnabled(partAuthoring);
  if (assemblyRibbon_) assemblyRibbon_->setEnabled(assemblyProductActive());
  if (assemblyInsertEscapeShortcut_)
    assemblyInsertEscapeShortcut_->setEnabled(assemblyProductActive());
  if (assemblyTreeView_)
    assemblyTreeView_->setEnabled(assemblyProductActive());
  if (componentTreeView_)
    componentTreeView_->setEnabled(component || componentAuthoring);
  if (ribbonStack_)
    ribbonStack_->setCurrentWidget(modelWorkspace
                                       ? static_cast<QWidget*>(modelRibbon_)
                                       : assemblyRibbon_);
  if (modelTreeStack_) {
    QWidget* tree = partTreePage_;
    if (component || componentAuthoring)
      tree = componentTreeView_;
    else if (assemblyProductActive())
      tree = assemblyTreeView_;
    modelTreeStack_->setCurrentWidget(tree);
  }
  if (historyDock_) historyDock_->setVisible(part);
  if (historyToggleAction_) historyToggleAction_->setEnabled(part);
  if (createAction_) createAction_->setEnabled(standalone);
  if (saveAction_)
    saveAction_->setEnabled(standalone || writableAssembly ||
                            writableComponent);
  if (saveProjectAsAction_)
    saveProjectAsAction_->setEnabled(standalone || writableAssembly ||
                                     writableComponent);
  if (importStepAction_) importStepAction_->setEnabled(standalone);
  if (exportStepAction_) exportStepAction_->setEnabled(standalone);
  if (exportStlAction_) exportStlAction_->setEnabled(standalone);
  if (exportStepAction_)
    exportStepAction_->setToolTip(
        writableAssembly
            ? QString::fromUtf8("Экспорт сборки будет добавлен на этапе A9")
            : QString{});
  if (exportStlAction_)
    exportStlAction_->setToolTip(
        writableAssembly
            ? QString::fromUtf8("Экспорт сборки будет добавлен на этапе A9")
            : QString{});
  if (upgradeProjectFormatAction_) upgradeProjectFormatAction_->setEnabled(standalone);
  if (convertToProductDocumentAction_)
    convertToProductDocumentAction_->setEnabled(standalone);
  if (relocateProductDocumentAction_)
    relocateProductDocumentAction_->setEnabled(standalone);
  if (!part) {
    if (undoAction_) undoAction_->setEnabled(false);
    if (redoAction_) redoAction_->setEnabled(false);
  }
  updateAssemblyRenameAvailability();
  updateCreateComponentFromBodyAvailability();
}

void MainWindow::selectComponentTreeIndex(const QModelIndex& current) {
  if ((!componentProductActive() && !rootComponentAuthoringActive()) ||
      componentSelectionReentry_ ||
      !componentTreeModel_ || !viewport_ || !viewport_->componentSession())
    return;
  const auto address = componentTreeModel_->addressForIndex(current);
  std::optional<ComponentInstanceAddress> instance;
  if (address && address->get().publication().isSameCurrentPublication(
                     viewport_->componentSession()->publicationHandle())) {
    std::visit(
        [&instance](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, ComponentTreeRootAddress> ||
                        std::is_same_v<T, ComponentTreeOccurrenceAddress> ||
                        std::is_same_v<T, ComponentTreeGroupAddress> ||
                        std::is_same_v<T, ComponentTreeOriginPlaneAddress> ||
                        std::is_same_v<T, ComponentTreeSketchAddress> ||
                        std::is_same_v<T,
                                       ComponentTreeReferenceImageAddress> ||
                        std::is_same_v<T, ComponentTreeDrawingAddress>)
            instance = value.instance;
          else if constexpr (std::is_same_v<T,
                                            ComponentTreeBodyAddress>)
            instance = value.body.component;
        },
        address->get().value());
  }
  std::optional<ComponentInstanceSelection> selection;
  if (instance)
    selection = ComponentInstanceSelection::create(
        viewport_->componentSession()->publicationHandle(), *instance);
  componentSelectionReentry_ = true;
  static_cast<void>(viewport_->setSelectedComponent(selection, false));
  componentSelectionReentry_ = false;
}

void MainWindow::selectComponentFromViewport(
    const ComponentInstanceSelection& component) {
  if ((!componentProductActive() && !rootComponentAuthoringActive()) ||
      componentSelectionReentry_ ||
      !componentTreeModel_ || !componentTreeView_ ||
      !componentTreeView_->selectionModel() || !viewport_ ||
      !viewport_->componentSession())
    return;
  const auto& selected = viewport_->selectedComponentView();
  if (!selected || selected->component() != component.component() ||
      !selected->isCurrentFor(component.publication()) ||
      !component.publication().isSameCurrentPublication(
          viewport_->componentSession()->publicationHandle()))
    return;
  const auto findInstance = [&](const auto& self,
                                const QModelIndex& parent) -> QModelIndex {
    const int rows = componentTreeModel_->rowCount(parent);
    for (int row = 0; row < rows; ++row) {
      const QModelIndex candidate = componentTreeModel_->index(row, 0, parent);
      const auto address = componentTreeModel_->addressForIndex(candidate);
      if (address && address->get().publication().isSameCurrentPublication(
                         component.publication())) {
        const auto* root = std::get_if<ComponentTreeRootAddress>(
            &address->get().value());
        const auto* occurrence = std::get_if<ComponentTreeOccurrenceAddress>(
            &address->get().value());
        if ((root && root->instance == component.component()) ||
            (occurrence && occurrence->instance == component.component()))
          return candidate;
      }
      const QModelIndex nested = self(self, candidate);
      if (nested.isValid()) return nested;
    }
    return {};
  };
  const QModelIndex index = findInstance(findInstance, QModelIndex{});
  if (!index.isValid()) return;
  const auto expectedPublication = component.publication();
  QPointer<MainWindow> receiver(this);
  componentSelectionReentry_ = true;
  const QSignalBlocker blocker(componentTreeView_->selectionModel());
  componentTreeView_->setCurrentIndex(index);
  componentTreeView_->scrollTo(index);
  if (!receiver) return;
  componentSelectionReentry_ = false;
  if (!viewport_ || !viewport_->componentSession() ||
      !expectedPublication.isSameCurrentPublication(
          viewport_->componentSession()->publicationHandle()))
    return;
  updateCreateComponentFromBodyAvailability();
}

void MainWindow::activateComponentDrawing(const QModelIndex& index) noexcept {
  const auto reject = [this](const QString& detail) {
    if (statusBar())
      statusBar()->showMessage(
          QString::fromUtf8("Не удалось открыть чертёж: ") + detail, 5000);
  };
  if (!componentProductActive() || !componentPersistenceTransitionIsQuiescent() ||
      !componentTreeModel_ || !componentTreeView_ || !viewport_ ||
      !viewport_->componentSession() || index.model() != componentTreeModel_) {
    reject(QString::fromUtf8("компонентный проект изменился"));
    return;
  }
  const auto node = componentTreeModel_->addressForIndex(index);
  if (!node) return;
  const auto* drawingAddress = std::get_if<ComponentTreeDrawingAddress>(
      &node->get().value());
  if (!drawingAddress) return;
  const ComponentScenePublicationHandle publication =
      node->get().publication();
  const ComponentViewportSession* expectedSession =
      viewport_->componentSession();
  if (!publication.isSameCurrentPublication(
          expectedSession->publicationHandle())) {
    reject(QString::fromUtf8("узел дерева устарел"));
    return;
  }
  const auto snapshot = publication.snapshotIfCurrent();
  if (!snapshot || drawingAddress->instance.epoch != snapshot->epoch()) {
    reject(QString::fromUtf8("публикация больше не активна"));
    return;
  }
  const auto& projection = snapshot->treeProjection();
  if (projection.productIncarnation != productDocument_.incarnation() ||
      projection.productIdentity != productDocument_.identity() ||
      projection.rootDefinitionId != productDocument_.rootDefinitionId()) {
    reject(QString::fromUtf8("представление не соответствует модели"));
    return;
  }
  const ComponentDefinition* definition =
      resolveComponentDefinition(productDocument_, drawingAddress->instance);
  const ComponentTreeDefinitionProjection* definitionProjection =
      projection.findDefinition(drawingAddress->instance.definitionId);
  if (!definition || !definitionProjection) {
    reject(QString::fromUtf8("компонент не найден"));
    return;
  }
  const ComponentDrawing* drawing =
      definition->findDrawing(drawingAddress->drawingId);
  const auto projectedDrawing = std::find_if(
      definitionProjection->drawings.begin(),
      definitionProjection->drawings.end(),
      [id = drawingAddress->drawingId](const auto& candidate) {
        return candidate.id == id;
      });
  if (!drawing || projectedDrawing == definitionProjection->drawings.end() ||
      !drawingMatchesProjection(*drawing, *projectedDrawing)) {
    reject(QString::fromUtf8("чертёж изменился или отсутствует"));
    return;
  }
  const DefinitionStateId expectedProjectedState =
      definitionProjection->stateId;
  if (definition->stateId() != expectedProjectedState) {
    reject(QString::fromUtf8("узел дерева устарел"));
    return;
  }

  const ProductIncarnationId expectedIncarnation =
      productDocument_.incarnation();
  const DefinitionId expectedRoot = productDocument_.rootDefinitionId();
  const auto expectedIdentity = productDocument_.identity();
  const ComponentTreeModel* expectedTree = componentTreeModel_;
  QPointer<MainWindow> receiver(this);
  drawing::DrawingSource source;
  try {
    source = drawing::collectDrawingSource(*drawing, definition->document());
  } catch (...) {
    reject(QString::fromUtf8("недостаточно ресурсов"));
    return;
  }
  if (!receiver) return;
  if (!source.succeeded()) {
    reject(source.status == drawing::DrawingSourceStatus::MissingSourceBody
               ? QString::fromUtf8("исходное тело не найдено")
               : QString::fromUtf8("некорректные параметры чертежа"));
    return;
  }

  const ComponentViewportSession* currentSession =
      viewport_ ? viewport_->componentSession() : nullptr;
  const ComponentDefinition* currentDefinition =
      resolveComponentDefinition(productDocument_, drawingAddress->instance);
  const ComponentDrawing* currentDrawing = currentDefinition
                                               ? currentDefinition->findDrawing(
                                                     drawingAddress->drawingId)
                                               : nullptr;
  const auto currentSnapshot = publication.snapshotIfCurrent();
  const ComponentTreeDefinitionProjection* currentDefinitionProjection =
      currentSnapshot
          ? currentSnapshot->treeProjection().findDefinition(
                drawingAddress->instance.definitionId)
          : nullptr;
  const auto currentProjectedDrawing =
      currentDefinitionProjection
          ? std::find_if(currentDefinitionProjection->drawings.begin(),
                         currentDefinitionProjection->drawings.end(),
                         [id = drawingAddress->drawingId](const auto& candidate) {
                           return candidate.id == id;
                         })
          : decltype(projectedDrawing){};
  if (!componentProductActive() || componentTreeModel_ != expectedTree ||
      !componentPersistenceTransitionIsQuiescent() ||
      currentSession != expectedSession || !currentSession ||
      !publication.isSameCurrentPublication(
          currentSession->publicationHandle()) ||
      productDocument_.incarnation() != expectedIncarnation ||
      productDocument_.rootDefinitionId() != expectedRoot ||
      productDocument_.identity() != expectedIdentity || !currentDefinition ||
      !currentDefinitionProjection ||
      currentDefinitionProjection->stateId != expectedProjectedState ||
      currentDefinition->stateId() != expectedProjectedState ||
      !currentDrawing ||
      currentProjectedDrawing == currentDefinitionProjection->drawings.end() ||
      !drawingMatchesProjection(*currentDrawing, *currentProjectedDrawing)) {
    reject(QString::fromUtf8("проект изменился во время подготовки"));
    return;
  }

  const QString title = QString::fromUtf8(
      currentDrawing->nameUtf8().data(),
      static_cast<qsizetype>(currentDrawing->nameUtf8().size()));
  drawingSheet_->setRetainedSource(
      std::move(source.shape), source.bodyCount, source.solidCount, title,
      currentDrawing->orientation() ==
          ComponentDrawingOrientation::Landscape,
      currentDrawing->scale());
  workspaceStack_->setCurrentWidget(drawingSheet_);
  statusBar()->showMessage(QString::fromUtf8("Чертёж открыт: ") + title,
                           3000);
}

void MainWindow::closeComponentDrawing() noexcept {
  if (!workspaceStack_ || !drawingSheet_ || !viewport_) return;
  if (workspaceStack_->currentWidget() == drawingSheet_)
    workspaceStack_->setCurrentWidget(viewport_);
}

void MainWindow::clearComponentDrawingWorkspace() noexcept {
  closeComponentDrawing();
  if (drawingSheet_) drawingSheet_->clearSource();
}

void MainWindow::enterComponentDefinitionEdit(const QModelIndex& index) {
  const bool switchingComponent = componentDefinitionEditContext_.has_value();
  if ((!componentProductActive() && !switchingComponent) ||
      !componentTreeModel_ || !componentTreeView_ || !viewport_ ||
      (!switchingComponent && !viewport_->componentSession()) ||
      index.model() != componentTreeModel_ ||
      partDesignCoordinator_.activeTool() != PartDesignToolKind::None ||
      partDesignCoordinator_.commitPending() ||
      partDesignCoordinator_.isReselecting() || editUiTransaction_ ||
      pendingNoChangeUiRestore_ || modelHistory_.isApplying() ||
      (workspaceStack_ && workspaceStack_->currentWidget() != viewport_))
    return;
  const auto node = componentTreeModel_->addressForIndex(index);
  if (!node) return;
  const auto* root =
      std::get_if<ComponentTreeRootAddress>(&node->get().value());
  const auto* occurrence =
      std::get_if<ComponentTreeOccurrenceAddress>(&node->get().value());
  if (!root && !occurrence) return;
  const ComponentInstanceAddress instance =
      root ? root->instance : occurrence->instance;
  if (switchingComponent &&
      componentDefinitionEditContext_->targetDefinitionId ==
          instance.definitionId &&
      componentDefinitionEditContext_->occurrencePath ==
          instance.occurrencePath)
    return;
  const bool rootAuthoring = instance.rootInstance();
  const ComponentDefinition* definition =
      resolveComponentDefinition(productDocument_, instance);
  if (!definition) return;

  std::unique_ptr<Document> working;
  try {
    working = std::make_unique<Document>(definition->document());
  } catch (...) {
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось открыть компонент для редактирования"),
        5000);
    return;
  }
  const ViewportNavigationState navigation = viewport_->navigationState();
  auto componentSession = switchingComponent
                              ? (rootComponentAuthoringActive()
                                     ? viewport_->takeComponentSession()
                                     : std::move(suspendedComponentSession_))
                              : viewport_->takeComponentSession();
  if (!componentSession) return;

  if (switchingComponent) {
    componentModelHistories_.insert_or_assign(
        componentDefinitionEditContext_->targetDefinitionId,
        std::move(modelHistory_));
  } else {
    suspendedComponentModelHistory_.emplace(std::move(modelHistory_));
  }
  const auto savedHistory = componentModelHistories_.find(
      instance.definitionId);
  if (savedHistory == componentModelHistories_.end()) {
    modelHistory_ = ModelCommandHistory{};
  } else {
    modelHistory_ = std::move(savedHistory->second);
    componentModelHistories_.erase(savedHistory);
  }
  componentDefinitionEditContext_.emplace(ComponentDefinitionEditContext{
      productDocument_.incarnation(), productDocument_.rootDefinitionId(),
      instance.definitionId, definition->stateId(), instance.occurrencePath,
      navigation.yawDeg, navigation.pitchDeg, navigation.zoom,
      navigation.pan, componentTreeModel_->data(index).toString()});
  componentEditDocument_ = std::move(working);
  if (!rootAuthoring)
    suspendedComponentSession_ = std::move(componentSession);
  invalidatePreviewUpdates();
  viewport_->resetScene();
  if (rootAuthoring)
    viewport_->replaceComponentSessionDuringEdit(std::move(componentSession));
  const auto scene = rootAuthoring
                         ? viewport_->componentSession()->scene()
                         : suspendedComponentSession_->scene();
  viewport_->setComponentEditContext(scene, instance);
  viewport_->setBox(activePartDocument().box());
  sketchViews_.clear();
  for (const auto& item : activePartDocument().sketches()) {
    sketchViews_.push_back({item.id});
    viewport_->addSketch(item.id, item.geometry,
                         sketchPresentationLabel(item), item.placement);
  }
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  refreshBodyViewFromDocument();
  workspaceStack_->setCurrentWidget(viewport_);
  applyProjectKind(EditorProjectKind::Part);
  updateComponentTreePresentation();
  definitionEditBreadcrumb_->setText(
      QString::fromUtf8("Активный компонент: %1")
          .arg(componentTreeModel_->data(index).toString()));
  definitionEditBreadcrumb_->show();
  definitionEditBackButton_->hide();
  statusBar()->showMessage(
      QString::fromUtf8("Компонент активен — доступна его история построений"),
      3500);
  updateUndoAvailability();
}

void MainWindow::leaveComponentDefinitionEdit() {
  if (!componentDefinitionEditContext_ || !componentEditDocument_ ||
      (!rootComponentAuthoringActive() && !suspendedComponentSession_) ||
      !viewport_)
    return;
  if (partDesignCoordinator_.activeTool() != PartDesignToolKind::None ||
      partDesignCoordinator_.commitPending() ||
      partDesignCoordinator_.isReselecting() || editUiTransaction_ ||
      pendingNoChangeUiRestore_ ||
      (workspaceStack_ && workspaceStack_->currentWidget() != viewport_)) {
    statusBar()->showMessage(
        QString::fromUtf8("Завершите или отмените активный инструмент"), 3000);
    return;
  }
  if (componentPresentationEpochClock_ ==
      std::numeric_limits<std::uint64_t>::max())
    return;
  const DefinitionId target =
      componentDefinitionEditContext_->targetDefinitionId;
  const std::optional<OccurrencePath> path =
      componentDefinitionEditContext_->occurrencePath;
  const ViewportNavigationState navigation{
      componentDefinitionEditContext_->yawDeg,
      componentDefinitionEditContext_->pitchDeg,
      componentDefinitionEditContext_->zoom,
      componentDefinitionEditContext_->pan};
  const ComponentWorkspaceInstanceKey instance{target, path};
  const AssemblyMeshProfile profile{
      viewport_->meshQuality() == ViewportMeshQuality::High
          ? AssemblyMeshQuality::High
          : AssemblyMeshQuality::Normal};
  auto prepared = ComponentProjectionPreparer::prepare(
      productDocument_,
      ProductPresentationEpoch{componentPresentationEpochClock_ + 1}, profile,
      ComponentWorkspaceSemanticState{instance, instance, std::nullopt,
                                      "component definition edit"},
      componentPreparationFailurePoint_, componentTreeFailurePoint_);
  if (!prepared || !prepared.projection) {
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось восстановить представление компонентов"),
        5000);
    return;
  }

  componentModelHistories_.insert_or_assign(target,
                                             std::move(modelHistory_));
  if (!suspendedComponentModelHistory_) std::terminate();
  modelHistory_ = std::move(*suspendedComponentModelHistory_);
  suspendedComponentModelHistory_.reset();
  invalidatePreviewUpdates();
  componentEditDocument_.reset();
  componentDefinitionEditContext_.reset();
  suspendedComponentSession_.reset();
  installPreparedComponentProjection(std::move(prepared.projection));
  viewport_->restoreNavigationState(navigation);
  updateComponentTreePresentation();
  applyProjectKind(EditorProjectKind::Part);
  if (definitionEditBreadcrumb_) definitionEditBreadcrumb_->hide();
  if (definitionEditBackButton_) {
    definitionEditBackButton_->setText(
        QString::fromUtf8("Вернуться в сборку"));
    definitionEditBackButton_->hide();
  }
  statusBar()->showMessage(QString::fromUtf8("Компоненты"), 2000);
  updateUndoAvailability();
}

void MainWindow::updateComponentTreePresentation() noexcept {
  try {
    if (!componentTreeModel_) return;
    const QString baseName = windowFilePath().isEmpty()
                                 ? QString::fromUtf8("Новый проект")
                                 : QFileInfo(windowFilePath()).completeBaseName();
    componentTreeModel_->setRootDisplayName(baseName);
    componentTreeModel_->setRootVisible(rootComponentVisible_);
    if (componentDefinitionEditContext_) {
      componentTreeModel_->setActiveComponent(
          componentDefinitionEditContext_->targetDefinitionId,
          componentDefinitionEditContext_->occurrencePath);
    } else {
      componentTreeModel_->clearActiveComponent();
    }
    if (viewport_) viewport_->setComponentRootVisible(rootComponentVisible_);
  } catch (...) {
  }
}

void MainWindow::connectComponentTreeVisibilitySignals() {
  if (!componentTreeModel_) return;
  connect(componentTreeModel_,
          &ComponentTreeModel::componentVisibilityChangeRequested, this,
          [this](ComponentInstanceAddress instance, bool visible) {
            QTimer::singleShot(
                0, this,
                [this, instance = std::move(instance), visible]() mutable {
                  setComponentVisibility(std::move(instance), visible);
                });
          });
  connect(componentTreeModel_,
          &ComponentTreeModel::nodeVisibilityChangeRequested, this,
          [this](ComponentTreeAddressValue address, bool visible) {
            QTimer::singleShot(
                0, this,
                [this, address = std::move(address), visible]() mutable {
                  setComponentTreeNodeVisibility(std::move(address), visible);
                });
          });
  connect(componentTreeModel_, &ComponentTreeModel::nodeRenameRequested, this,
          [this](ComponentTreeAddressValue address, QString name) {
            QMetaObject::invokeMethod(
                this,
                [this, address = std::move(address),
                 name = std::move(name)]() mutable {
                  renameComponentTreeItem(address, name);
                },
                Qt::QueuedConnection);
          });
}

void MainWindow::setComponentTreeNodeVisibility(
    ComponentTreeAddressValue address, bool visible) {
  if ((!componentProductActive() && !componentDefinitionEditContext_) ||
      !viewport_)
    return;

  std::optional<ComponentInstanceAddress> instance;
  std::visit(
      [&instance](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ComponentTreeGroupAddress> ||
                      std::is_same_v<T, ComponentTreeOriginPlaneAddress> ||
                      std::is_same_v<T, ComponentTreeSketchAddress> ||
                      std::is_same_v<T, ComponentTreeReferenceImageAddress> ||
                      std::is_same_v<T, ComponentTreeDrawingAddress>)
          instance = value.instance;
        else if constexpr (std::is_same_v<T, ComponentTreeBodyAddress>)
          instance = value.body.component;
      },
      address);
  if (!instance) return;

  const bool activeInstance =
      componentDefinitionEditContext_ &&
      componentDefinitionEditContext_->targetDefinitionId ==
          instance->definitionId &&
      componentDefinitionEditContext_->occurrencePath ==
          instance->occurrencePath;

  if (const auto* group =
          std::get_if<ComponentTreeGroupAddress>(&address)) {
    if (activeInstance && group->group == ComponentTreeGroupKind::Origin) {
      viewport_->setOriginVisible(visible);
      for (int plane = 0; plane < 3; ++plane)
        viewport_->setBasePlaneVisible(plane, visible);
      return;
    }
    if (activeInstance && group->group == ComponentTreeGroupKind::Sketches) {
      for (std::size_t index = 0; index < sketchViews_.size(); ++index)
        viewport_->setSketchVisible(index, visible);
      return;
    }
    if (group->group == ComponentTreeGroupKind::Drawings) return;
  }
  if (const auto* plane =
          std::get_if<ComponentTreeOriginPlaneAddress>(&address)) {
    if (!activeInstance) return;
    viewport_->setBasePlaneVisible(
        static_cast<int>(plane->plane), visible);
    return;
  }
  if (const auto* sketch =
          std::get_if<ComponentTreeSketchAddress>(&address)) {
    if (!activeInstance) return;
    const auto found = std::find_if(
        sketchViews_.begin(), sketchViews_.end(),
        [id = sketch->sketchId](const SketchViewEntry& entry) {
          return entry.documentSketchId == id;
        });
    if (found != sketchViews_.end())
      viewport_->setSketchVisible(
          static_cast<std::size_t>(found - sketchViews_.begin()), visible);
    return;
  }
  if (std::holds_alternative<ComponentTreeDrawingAddress>(address)) return;

  const auto mutate = [&address, visible](Document& document) {
    bool changed = false;
    if (const auto* bodyAddress =
            std::get_if<ComponentTreeBodyAddress>(&address)) {
      Body* body = document.findBody(bodyAddress->body.bodyId);
      if (body && body->visible() != visible) {
        body->setVisible(visible);
        changed = true;
      }
    } else if (const auto* imageAddress =
                   std::get_if<ComponentTreeReferenceImageAddress>(&address)) {
      ReferenceImage* image =
          document.findReferenceImage(imageAddress->imageId);
      if (image && image->visible != visible) {
        image->visible = visible;
        changed = true;
      }
    } else if (const auto* group =
                   std::get_if<ComponentTreeGroupAddress>(&address)) {
      if (group->group == ComponentTreeGroupKind::Bodies) {
        for (const Body& candidate : document.bodies()) {
          Body* body = document.findBody(candidate.id());
          if (body && body->visible() != visible) {
            body->setVisible(visible);
            changed = true;
          }
        }
      } else if (group->group == ComponentTreeGroupKind::Images) {
        for (const ReferenceImage& candidate : document.referenceImages()) {
          ReferenceImage* image = document.findReferenceImage(candidate.id);
          if (image && image->visible != visible) {
            image->visible = visible;
            changed = true;
          }
        }
      }
    }
    return changed;
  };

  if (activeInstance) {
    const HistorySelectionState previousSelection = captureHistorySelection();
    Document previous = activePartDocument();
    if (!mutate(mutableStandaloneDocument())) return;
    if (!pushModelTransition(std::move(previous), previousSelection,
                             captureCommittedEndState()))
      return;
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    completeModelTransitionUi();
    return;
  }

  const ComponentDefinition* definition =
      resolveComponentDefinition(productDocument_, *instance);
  if (!definition ||
      componentPresentationEpochClock_ ==
          std::numeric_limits<std::uint64_t>::max())
    return;
  Document candidate;
  try {
    candidate = definition->document();
  } catch (...) {
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось подготовить изменение видимости"),
        4000);
    return;
  }
  if (!mutate(candidate)) return;

  const ComponentWorkspaceInstanceKey selected{
      instance->definitionId, instance->occurrencePath};
  const AssemblyMeshProfile profile{
      viewport_->meshQuality() == ViewportMeshQuality::High
          ? AssemblyMeshQuality::High
          : AssemblyMeshQuality::Normal};
  ComponentEditProjectionSuccessor successor{
      ProductPresentationEpoch{componentPresentationEpochClock_ + 1}, profile,
      ComponentWorkspaceSemanticState{selected, selected, std::nullopt,
                                      "tree visibility"},
      componentPreparationFailurePoint_, componentTreeFailurePoint_};
  const auto result = productDocument_.applyDefinitionEdit(
      {productDocument_.incarnation(), productDocument_.rootDefinitionId(),
       instance->definitionId,
       productDocument_.definitionStateId(instance->definitionId)},
      candidate, successor);
  if (result.status != DefinitionEditStatus::Committed) {
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось изменить видимость элемента"), 4000);
    return;
  }
  auto projection = successor.release();
  if (!projection) std::terminate();
  if (componentDefinitionEditContext_)
    installActiveComponentProjection(std::move(projection));
  else
    installPreparedComponentProjection(std::move(projection));
  setWindowModified(true);
  updateUndoAvailability();
}

bool MainWindow::mutateComponentDocumentFromTree(
    const ComponentInstanceAddress& instance,
    const std::function<bool(Document&)>& mutation,
    const QString& failureMessage) {
  if (!mutation || !viewport_ ||
      (!componentProductActive() && !componentDefinitionEditContext_))
    return false;
  const bool activeInstance =
      componentDefinitionEditContext_ &&
      componentDefinitionEditContext_->targetDefinitionId ==
          instance.definitionId &&
      componentDefinitionEditContext_->occurrencePath ==
          instance.occurrencePath;
  if (activeInstance) {
    if (partDesignCoordinator_.activeTool() != PartDesignToolKind::None ||
        partDesignCoordinator_.commitPending() ||
        partDesignCoordinator_.isReselecting()) {
      statusBar()->showMessage(
          QString::fromUtf8("Завершите или отмените активный инструмент"),
          3000);
      return false;
    }
    const HistorySelectionState previousSelection = captureHistorySelection();
    Document previous = activePartDocument();
    if (!mutation(mutableStandaloneDocument())) return false;
    if (!pushModelTransition(std::move(previous), previousSelection,
                             captureCommittedEndState()))
      return false;
    syncSketchPresentationFromDocument();
    refreshBodyViewFromDocument();
    rebuildFeatureTree();
    rebuildHistoryPanel();
    completeModelTransitionUi();
    return true;
  }

  const ComponentDefinition* definition =
      resolveComponentDefinition(productDocument_, instance);
  if (!definition ||
      componentPresentationEpochClock_ ==
          std::numeric_limits<std::uint64_t>::max())
    return false;
  Document candidate;
  try {
    candidate = definition->document();
  } catch (...) {
    statusBar()->showMessage(failureMessage, 4000);
    return false;
  }
  if (!mutation(candidate)) return false;
  const ComponentWorkspaceInstanceKey selected{
      instance.definitionId, instance.occurrencePath};
  const AssemblyMeshProfile profile{
      viewport_->meshQuality() == ViewportMeshQuality::High
          ? AssemblyMeshQuality::High
          : AssemblyMeshQuality::Normal};
  ComponentEditProjectionSuccessor successor{
      ProductPresentationEpoch{componentPresentationEpochClock_ + 1}, profile,
      ComponentWorkspaceSemanticState{selected, selected, std::nullopt,
                                      "tree edit"},
      componentPreparationFailurePoint_, componentTreeFailurePoint_};
  const auto result = productDocument_.applyDefinitionEdit(
      {productDocument_.incarnation(), productDocument_.rootDefinitionId(),
       instance.definitionId,
       productDocument_.definitionStateId(instance.definitionId)},
      candidate, successor);
  if (result.status != DefinitionEditStatus::Committed) {
    statusBar()->showMessage(failureMessage, 4000);
    return false;
  }
  auto projection = successor.release();
  if (!projection) std::terminate();
  if (componentDefinitionEditContext_)
    installActiveComponentProjection(std::move(projection));
  else
    installPreparedComponentProjection(std::move(projection));
  setWindowModified(true);
  updateUndoAvailability();
  return true;
}

void MainWindow::renameComponentTreeItem(
    const ComponentTreeAddressValue& address, const QString& requestedName) {
  std::optional<ComponentInstanceAddress> instance;
  QString currentName;
  if (const auto* occurrenceAddress =
          std::get_if<ComponentTreeOccurrenceAddress>(&address)) {
    instance = occurrenceAddress->instance;
    const ComponentOccurrence* occurrence =
        instance->occurrencePath
            ? resolveComponentOccurrence(productDocument_,
                                         *instance->occurrencePath)
            : nullptr;
    if (occurrence)
      currentName = QString::fromStdString(occurrence->displayNameUtf8());
  } else if (const auto* bodyAddress =
          std::get_if<ComponentTreeBodyAddress>(&address)) {
    instance = bodyAddress->body.component;
    const ComponentDefinition* definition =
        resolveComponentDefinition(productDocument_, *instance);
    const Body* body = definition
                           ? definition->document().findBody(
                                 bodyAddress->body.bodyId)
                           : nullptr;
    if (body) currentName = QString::fromStdString(body->name());
  } else if (const auto* sketchAddress =
                 std::get_if<ComponentTreeSketchAddress>(&address)) {
    instance = sketchAddress->instance;
    const ComponentDefinition* definition =
        resolveComponentDefinition(productDocument_, *instance);
    const DocumentSketch* sketch =
        definition
            ? definition->document().findSketch(sketchAddress->sketchId)
            : nullptr;
    if (sketch) currentName = QString::fromStdString(sketch->name);
  } else if (const auto* imageAddress =
                 std::get_if<ComponentTreeReferenceImageAddress>(&address)) {
    instance = imageAddress->instance;
    const ComponentDefinition* definition =
        resolveComponentDefinition(productDocument_, *instance);
    const ReferenceImage* image =
        definition ? definition->document().findReferenceImage(
                         imageAddress->imageId)
                   : nullptr;
    if (image) currentName = QString::fromStdString(image->name);
  }
  if (!instance) return;
  const QString requested = requestedName.trimmed();
  if (requested.isEmpty() || requested == currentName)
    return;
  const std::string name = requested.trimmed().toUtf8().toStdString();
  if (const auto* occurrenceAddress =
          std::get_if<ComponentTreeOccurrenceAddress>(&address)) {
    if (!occurrenceAddress->instance.occurrencePath || !viewport_ ||
        componentPresentationEpochClock_ ==
            std::numeric_limits<std::uint64_t>::max())
      return;
    const ComponentWorkspaceInstanceKey selected =
        componentDefinitionEditContext_
            ? ComponentWorkspaceInstanceKey{
                  componentDefinitionEditContext_->targetDefinitionId,
                  componentDefinitionEditContext_->occurrencePath}
            : ComponentWorkspaceInstanceKey{
                  productDocument_.rootDefinitionId(), std::nullopt};
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    ComponentEditProjectionSuccessor successor{
        ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
        profile,
        ComponentWorkspaceSemanticState{selected, selected, std::nullopt,
                                        "component rename"},
        componentPreparationFailurePoint_, componentTreeFailurePoint_};
    const auto status = productDocument_.renameComponentOccurrence(
        *occurrenceAddress->instance.occurrencePath, name, successor);
    if (status != ComponentOccurrenceEditStatus::Committed) {
      statusBar()->showMessage(
          QString::fromUtf8("Не удалось переименовать компонент"), 4000);
      return;
    }
    auto projection = successor.release();
    if (!projection) std::terminate();
    if (componentDefinitionEditContext_)
      installActiveComponentProjection(std::move(projection));
    else
      installPreparedComponentProjection(std::move(projection));
    setWindowModified(true);
    statusBar()->showMessage(QString::fromUtf8("Компонент переименован"),
                             2500);
    return;
  }
  const auto mutation = [&address, &name](Document& document) {
    if (const auto* bodyAddress =
            std::get_if<ComponentTreeBodyAddress>(&address)) {
      Body* body = document.findBody(bodyAddress->body.bodyId);
      if (!body) return false;
      body->setName(name);
      return true;
    }
    if (const auto* sketchAddress =
            std::get_if<ComponentTreeSketchAddress>(&address)) {
      DocumentSketch* sketch = document.findSketch(sketchAddress->sketchId);
      if (!sketch) return false;
      sketch->name = name;
      return true;
    }
    const auto* imageAddress =
        std::get_if<ComponentTreeReferenceImageAddress>(&address);
    ReferenceImage* image = imageAddress
                                ? document.findReferenceImage(
                                      imageAddress->imageId)
                                : nullptr;
    if (!image) return false;
    image->name = name;
    return true;
  };
  if (mutateComponentDocumentFromTree(
          *instance, mutation,
          QString::fromUtf8("Не удалось переименовать элемент")))
    statusBar()->showMessage(QString::fromUtf8("Элемент переименован"), 2500);
}

void MainWindow::deleteComponentTreeItem(
    const ComponentTreeAddressValue& address) {
  std::optional<ComponentInstanceAddress> instance;
  if (const auto* occurrence =
          std::get_if<ComponentTreeOccurrenceAddress>(&address))
    instance = occurrence->instance;
  else if (const auto* body =
               std::get_if<ComponentTreeBodyAddress>(&address))
    instance = body->body.component;
  else if (const auto* sketch =
               std::get_if<ComponentTreeSketchAddress>(&address))
    instance = sketch->instance;
  else if (const auto* image =
               std::get_if<ComponentTreeReferenceImageAddress>(&address))
    instance = image->instance;
  if (!instance) return;
  if (QMessageBox::question(
          this, QString::fromUtf8("Удаление"),
          QString::fromUtf8("Удалить выбранный элемент и зависимые построения?"),
          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) !=
      QMessageBox::Yes)
    return;
  if (const auto* occurrence =
          std::get_if<ComponentTreeOccurrenceAddress>(&address)) {
    if (!occurrence->instance.occurrencePath || !viewport_ ||
        componentPresentationEpochClock_ ==
            std::numeric_limits<std::uint64_t>::max())
      return;
    if (componentDefinitionEditContext_) {
      const auto& target = occurrence->instance.occurrencePath->entries();
      const auto& active = componentDefinitionEditContext_->occurrencePath;
      if (active && active->entries().size() >= target.size() &&
          std::equal(target.begin(), target.end(), active->entries().begin())) {
        statusBar()->showMessage(
            QString::fromUtf8(
                "Нельзя удалить активный компонент или его родителя"),
            4000);
        return;
      }
    }
    const ComponentWorkspaceInstanceKey selected =
        componentDefinitionEditContext_
            ? ComponentWorkspaceInstanceKey{
                  componentDefinitionEditContext_->targetDefinitionId,
                  componentDefinitionEditContext_->occurrencePath}
            : ComponentWorkspaceInstanceKey{
                  productDocument_.rootDefinitionId(), std::nullopt};
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    ComponentEditProjectionSuccessor successor{
        ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
        profile,
        ComponentWorkspaceSemanticState{selected, selected, std::nullopt,
                                        "component remove"},
        componentPreparationFailurePoint_, componentTreeFailurePoint_};
    const auto status = productDocument_.removeComponentOccurrence(
        *occurrence->instance.occurrencePath, successor);
    if (status != ComponentOccurrenceEditStatus::Committed) {
      statusBar()->showMessage(
          QString::fromUtf8("Не удалось удалить компонент"), 4000);
      return;
    }
    auto projection = successor.release();
    if (!projection) std::terminate();
    if (componentDefinitionEditContext_)
      installActiveComponentProjection(std::move(projection));
    else
      installPreparedComponentProjection(std::move(projection));
    setWindowModified(true);
    statusBar()->showMessage(QString::fromUtf8("Компонент удалён"), 2500);
    return;
  }
  const auto mutation = [&address](Document& document) {
    std::string error;
    if (const auto* bodyAddress =
            std::get_if<ComponentTreeBodyAddress>(&address)) {
      const FeatureRemovalPlan plan =
          document.planBodyRemoval(bodyAddress->body.bodyId);
      if (!plan.applicable) return false;
      const auto operands = removedJoinOperandIds(document, plan);
      if (!document.applyRemovalPlan(plan, &error)) return false;
      releaseUnconsumedJoinOperands(document, operands);
      return true;
    }
    if (const auto* sketchAddress =
            std::get_if<ComponentTreeSketchAddress>(&address)) {
      const FeatureRemovalPlan plan =
          document.planSketchRemoval(sketchAddress->sketchId);
      return plan.applicable && document.applyRemovalPlan(plan, &error);
    }
    const auto* imageAddress =
        std::get_if<ComponentTreeReferenceImageAddress>(&address);
    return imageAddress && document.removeReferenceImage(imageAddress->imageId);
  };
  if (mutateComponentDocumentFromTree(
          *instance, mutation,
          QString::fromUtf8("Не удалось удалить элемент")))
    statusBar()->showMessage(QString::fromUtf8("Элемент удалён"), 2500);
}

void MainWindow::setComponentVisibility(ComponentInstanceAddress instance,
                                        bool visible) {
  if ((!componentProductActive() && !rootComponentAuthoringActive()) ||
      !componentTreeModel_ || !viewport_ ||
      !viewport_->componentSession())
    return;
  if (!instance.occurrencePath) {
    if (instance.definitionId != productDocument_.rootDefinitionId()) return;
    rootComponentVisible_ = visible;
    updateComponentTreePresentation();
    return;
  }
  if (componentPresentationEpochClock_ ==
      std::numeric_limits<std::uint64_t>::max())
    return;
  const ComponentWorkspaceInstanceKey selected{
      instance.definitionId, instance.occurrencePath};
  const AssemblyMeshProfile profile{
      viewport_->meshQuality() == ViewportMeshQuality::High
          ? AssemblyMeshQuality::High
          : AssemblyMeshQuality::Normal};
  ModelWorkspaceProjectionSuccessor successor(
      ProductPresentationEpoch{componentPresentationEpochClock_ + 1}, profile,
      ComponentWorkspaceSemanticState{selected, selected, std::nullopt,
                                      "component visibility"},
      componentExtractionProjectionFailurePoint_);
  const auto status = productDocument_.setComponentOccurrenceVisible(
      *instance.occurrencePath, visible, successor);
  if (status == ComponentVisibilityStatus::NoChange) {
    updateComponentTreePresentation();
    return;
  }
  if (status != ComponentVisibilityStatus::Committed) {
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось изменить видимость компонента"), 4000);
    updateComponentTreePresentation();
    return;
  }
  auto projection = successor.takeComponent();
  if (!projection) std::terminate();
  if (rootComponentAuthoringActive())
    installActiveComponentProjection(std::move(projection));
  else
    installPreparedComponentProjection(std::move(projection));
  setWindowModified(true);
  updateUndoAvailability();
}

void MainWindow::selectAssemblyTreeIndex(const QModelIndex& current) {
  if (projectKind_ != EditorProjectKind::AssemblyReadOnly ||
      assemblySelectionReentry_ || !assemblyTreeModel_ || !viewport_)
    return;
  ++assemblyTreeSelectionCallbackCountForTesting_;
  if (assemblyTreeSelectionCallbackResourceFailureForTesting_) return;
  const auto address = assemblyTreeModel_->addressForIndex(current);
  if (!address || address->get().kind != AssemblyTreeNodeKind::Occurrence ||
      !address->get().occurrence) {
    assemblySelectionReentry_ = true;
    static_cast<void>(viewport_->setSelectedAssemblyOccurrence(std::nullopt,
                                                                false));
    assemblySelectionReentry_ = false;
    updateAssemblyRenameAvailability();
    return;
  }
  assemblySelectionReentry_ = true;
  static_cast<void>(
      viewport_->setSelectedAssemblyOccurrence(address->get().occurrence,
                                                false));
  assemblySelectionReentry_ = false;
  updateAssemblyRenameAvailability();
}

void MainWindow::selectAssemblyOccurrenceFromViewport(
    const AssemblyOccurrenceSelection& occurrence) {
  if (projectKind_ != EditorProjectKind::AssemblyReadOnly ||
      assemblySelectionReentry_ || !assemblyTreeModel_ || !assemblyTreeView_ ||
      !viewport_)
    return;
  const auto& selected = viewport_->selectedAssemblyOccurrenceView();
  if (!selected ||
      selected->occurrence != occurrence.occurrence ||
      !selected->isCurrentFor(occurrence.publication))
    return;
  const QModelIndex index = assemblyTreeModel_->indexForOccurrence(occurrence);
  if (!index.isValid()) return;
  assemblySelectionReentry_ = true;
  const QSignalBlocker blocker(assemblyTreeView_->selectionModel());
  assemblyTreeView_->setCurrentIndex(index);
  assemblyTreeView_->scrollTo(index);
  assemblySelectionReentry_ = false;
  updateAssemblyRenameAvailability();
}

void MainWindow::commitComponentReplacement(
    std::unique_ptr<PreparedComponentWorkspace> prepared,
    const QString& path) noexcept {
  if (!prepared || !prepared->epoch().isValid()) return;

  clearComponentDrawingWorkspace();

  cancelAssemblyInsertBeforeReplacement();
  preparePartDesignForReplacement();
  detachDefinitionEditForReplacement();
  detachAssemblyInsertUi(true);
  clearAssemblyCommandHistoryForReplacement();
  if (modelRibbon_) modelRibbon_->clearActiveTool();
  finishEditUiTransaction();
  mirrorPresentationBefore_.reset();
  historicalLegacyExtrusionSourceSketchId_.reset();
  selectedExtrusionSource_.reset();
  currentSketchFaceReference_.reset();
  selectedReferenceImageId_ = kInvalidReferenceImageId;
  referenceImageEditBefore_.reset();
  referenceImageEditSelectionBefore_.reset();
  pendingReferenceImagePath_.clear();
  pendingReferenceImagePixelSize_ = {};
  sketchViews_.clear();
  editingSketchIndex_.reset();

  QObject::disconnect(assemblyTreeSelectionConnection_);
  assemblyTreeSelectionConnection_ = {};
  QObject::disconnect(componentTreeSelectionConnection_);
  componentTreeSelectionConnection_ = {};
  if (assemblyTreeView_) assemblyTreeView_->setModel(nullptr);
  if (componentTreeView_) componentTreeView_->setModel(nullptr);
  delete assemblyTreeModel_;
  assemblyTreeModel_ = nullptr;
  delete componentTreeModel_;
  componentTreeModel_ = nullptr;
  viewport_->resetScene();
  modelHistory_.clear(true);
  if (historyLayout_) {
    while (QLayoutItem* item = historyLayout_->takeAt(0)) {
      delete item->widget();
      delete item;
    }
  }
  historySteps_.clear();
  historyPosition_ = 0;
  if (historyTimeline_) {
    const QSignalBlocker blocker(historyTimeline_);
    historyTimeline_->setStepCount(0);
    historyTimeline_->setPosition(0);
  }

  productDocument_ = prepared->releaseProduct();
  rootComponentVisible_ = true;
  linkedPartRuntimeStates_.clear();
  mateRuntimeDiagnostics_.clear();
  mateDiagnosticsValidityStamp_.reset();
  projectKind_ = EditorProjectKind::Part;
  if (prepared->persistenceState)
    projectPersistenceState_ = *prepared->persistenceState;
  auto session = prepared->releaseSession();
  auto tree = prepared->releaseTreeModel();
  componentPresentationEpochClock_ = prepared->epoch().value();

  componentTreeModel_ = tree.release();
  componentTreeModel_->setParent(componentTreeView_);
  connectComponentTreeVisibilitySignals();
  componentTreeView_->setModel(componentTreeModel_);
  componentTreeView_->expandAll();
  componentTreeSelectionConnection_ = connect(
      componentTreeView_->selectionModel(),
      &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        QPointer<MainWindow> receiver(this);
        selectComponentTreeIndex(current);
        if (!receiver) return;
        updateCreateComponentFromBodyAvailability();
      });
  viewport_->installComponentSession(std::move(session));
  workspaceStack_->setCurrentWidget(viewport_);
  applyProjectKind(EditorProjectKind::Part);
  setProjectPath(path);
  setWindowModified(false);
  updateUndoAvailability();
  updateUpgradeProjectFormatAction();
  statusBar()->showMessage(QString::fromUtf8("Компонент открыт"), 4000);
}

std::unique_ptr<PreparedComponentProjection>
MainWindow::prepareComponentIdentityProjection(
    ComponentIdentityTransition transition) noexcept {
  if (!componentPersistenceTransitionIsQuiescent() ||
      !transition.isCurrentSource(productDocument_) ||
      componentPresentationEpochClock_ ==
          std::numeric_limits<std::uint64_t>::max() ||
      !viewport_ || !viewport_->componentSession())
    return {};
  try {
    const auto* incumbent = viewport_->componentSession();
    ComponentWorkspaceSemanticState semantic{
        {productDocument_.rootDefinitionId(), std::nullopt}, std::nullopt,
        std::nullopt, {}};
    std::optional<ComponentWorkspaceInstanceKey> hovered;
    if (incumbent->selectedComponent()) {
      const auto& address = incumbent->selectedComponent()->component();
      semantic.selectedInstance =
          ComponentWorkspaceInstanceKey{address.definitionId,
                                        address.occurrencePath};
    }
    if (incumbent->hoveredComponent()) {
      const auto& address = incumbent->hoveredComponent()->component();
      hovered = ComponentWorkspaceInstanceKey{address.definitionId,
                                              address.occurrencePath};
    }
    const ProductPresentationEpoch epoch{
        componentPresentationEpochClock_ + 1};
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    auto prepared = ComponentProjectionPreparer::prepareProjectedIdentity(
        productDocument_, epoch, profile, semantic, hovered,
        std::move(transition), componentPreparationFailurePoint_,
        componentTreeFailurePoint_);
    if (!prepared || !prepared.projection ||
        !prepared.projection->session() ||
        !prepared.projection->treeModel())
      return {};
    const auto snapshot =
        prepared.projection->session()->publicationHandle().snapshotIfCurrent();
    if (!snapshot || !prepared.projection->identityTransition() ||
        snapshot->treeProjection().productIdentity !=
            std::optional<ProductDocumentIdentity>{
                prepared.projection->identityTransition()->targetIdentity()})
      return {};
    return std::move(prepared.projection);
  } catch (...) {
    return {};
  }
}

void MainWindow::installComponentIdentityProjection(
    std::unique_ptr<PreparedComponentProjection> projection) noexcept {
  installPreparedComponentProjection(std::move(projection));
}

void MainWindow::installPreparedComponentProjection(
    std::unique_ptr<PreparedComponentProjection> projection) noexcept {
  if (!projection) return;
  const QSet<QString> expanded =
      captureExpandedComponentTree(componentTreeView_, componentTreeModel_);
  clearComponentDrawingWorkspace();
  // The exact carrier/identity/publication seal was validated before the
  // writer.  The synchronous service committed that same seal, so this tail
  // deliberately performs no ProductDocument or snapshot query that could
  // allocate or fail after durable publication.
  QObject::disconnect(componentTreeSelectionConnection_);
  componentTreeSelectionConnection_ = {};
  if (componentTreeView_) componentTreeView_->setModel(nullptr);
  // Retire the old exact publication before any callback can observe the new
  // product identity through the stale tree/session pair.
  viewport_->clearComponentSession();
  delete componentTreeModel_;
  componentTreeModel_ = nullptr;

  auto session = projection->releaseSession();
  auto tree = projection->releaseTreeModel();
  componentPresentationEpochClock_ = projection->epoch().value();
  componentTreeModel_ = tree.release();
  componentTreeModel_->setParent(componentTreeView_);
  connectComponentTreeVisibilitySignals();
  componentTreeView_->setModel(componentTreeModel_);
  restoreExpandedComponentTree(componentTreeView_, componentTreeModel_,
                               expanded);
  componentTreeSelectionConnection_ = connect(
      componentTreeView_->selectionModel(),
      &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        QPointer<MainWindow> receiver(this);
        selectComponentTreeIndex(current);
        if (!receiver) return;
        updateCreateComponentFromBodyAvailability();
      });
  viewport_->installComponentSession(std::move(session));
  updateComponentTreePresentation();

  // Critical sealed install ends above. This best-effort widget-only phase
  // intentionally does not call componentProductActive(), ProductDocument,
  // snapshot lookup or transition validation. It projects the selection that
  // was already prepared into the newly installed tree/view.
  const auto& semantic = projection->semanticState();
  const auto& selected = viewport_->selectedComponentView();
  if (!componentTreeModel_ || !componentTreeView_ ||
      !componentTreeView_->selectionModel() ||
      !viewport_->componentSession())
    return;
  const auto findPreparedInstance = [&](const auto& self,
                                        const QModelIndex& parent)
      -> QModelIndex {
    const int rows = componentTreeModel_->rowCount(parent);
    for (int row = 0; row < rows; ++row) {
      const QModelIndex candidate =
          componentTreeModel_->index(row, 0, parent);
      const auto address = componentTreeModel_->addressForIndex(candidate);
      if (address && address->get().publication().isSameCurrentPublication(
                         viewport_->componentSession()->publicationHandle())) {
        if (semantic.selectedBody) {
          const auto* body = std::get_if<ComponentTreeBodyAddress>(
              &address->get().value());
          if (body &&
              body->body.component.definitionId ==
                  semantic.selectedBody->instance.definitionId &&
              body->body.component.occurrencePath ==
                  semantic.selectedBody->instance.path &&
              body->body.bodyId == semantic.selectedBody->bodyId)
            return candidate;
        } else if (selected) {
          const auto* root = std::get_if<ComponentTreeRootAddress>(
              &address->get().value());
          const auto* occurrence =
              std::get_if<ComponentTreeOccurrenceAddress>(
                  &address->get().value());
          if ((root && root->instance == selected->component()) ||
              (occurrence && occurrence->instance == selected->component()))
            return candidate;
        }
      }
      const QModelIndex nested = self(self, candidate);
      if (nested.isValid()) return nested;
    }
    return {};
  };
  const QModelIndex preparedIndex =
      findPreparedInstance(findPreparedInstance, QModelIndex{});
  if (!preparedIndex.isValid()) return;
  componentSelectionReentry_ = true;
  const QSignalBlocker blocker(componentTreeView_->selectionModel());
  componentTreeView_->setCurrentIndex(preparedIndex);
  componentTreeView_->scrollTo(preparedIndex);
  componentSelectionReentry_ = false;
}

void MainWindow::installPreparedPartProjection(
    std::unique_ptr<PartWorkspaceProjection> projection) noexcept {
  if (!projection || !projection->bodyDisplay()) return;
  clearComponentDrawingWorkspace();
  componentPresentationEpochClock_ = projection->epoch().value();
  QObject::disconnect(componentTreeSelectionConnection_);
  componentTreeSelectionConnection_ = {};
  if (componentTreeView_) componentTreeView_->setModel(nullptr);
  viewport_->resetScene();
  delete componentTreeModel_;
  componentTreeModel_ = nullptr;
  auto display = projection->releaseBodyDisplay();
  if (!display || !viewport_->installPreparedBodyDisplay(std::move(*display)))
    std::terminate();
  // Extraction never mutates the hidden ordinary-Part tree/history widgets.
  // Reuse those cold views on the strict inverse instead of rebuilding them
  // after the model commit; all geometry/topology/mesh work is already owned
  // by PartWorkspaceProjection.
  if (projection->semanticState().selectedBody) {
    const BodyId bodyId = projection->semanticState().selectedBody->bodyId;
    bool selected = false;
    for (int rootIndex = 0; rootIndex < featureTree_->topLevelItemCount();
         ++rootIndex) {
      auto* root = featureTree_->topLevelItem(rootIndex);
      for (int groupIndex = 0;
           !selected && groupIndex < root->childCount(); ++groupIndex) {
        auto* group = root->child(groupIndex);
        for (int itemIndex = 0; itemIndex < group->childCount(); ++itemIndex) {
          auto* item = group->child(itemIndex);
          if (item->data(0, Qt::UserRole).toInt() != 3 ||
              static_cast<BodyId>(
                  item->data(0, Qt::UserRole + 2).toULongLong()) != bodyId)
            continue;
          const QSignalBlocker blocker(featureTree_);
          featureTree_->setCurrentItem(item);
          selected = true;
          break;
        }
      }
      if (selected) break;
    }
  }
  workspaceStack_->setCurrentWidget(viewport_);
}

std::optional<ComponentWorkspaceBodyKey>
MainWindow::selectedComponentExtractionBody() const noexcept {
  try {
    const auto selectedTreeBody = [this]()
        -> std::optional<ComponentWorkspaceBodyKey> {
      if (!componentTreeModel_ || !componentTreeView_ ||
          !componentTreeView_->currentIndex().isValid())
        return std::nullopt;
      const auto address = componentTreeModel_->addressForIndex(
          componentTreeView_->currentIndex());
      if (!address) return std::nullopt;
      const ComponentViewportSession* session =
          viewport_ ? viewport_->componentSession() : nullptr;
      if (!session && suspendedComponentSession_)
        session = suspendedComponentSession_.get();
      if (!session || !address->get().publication().isSameCurrentPublication(
                          session->publicationHandle()))
        return std::nullopt;
      const auto* body =
          std::get_if<ComponentTreeBodyAddress>(&address->get().value());
      if (!body || body->body.component.rootDefinitionId !=
                       productDocument_.rootDefinitionId())
        return std::nullopt;
      const auto* document = productDocument_.componentDocument(
          body->body.component.definitionId);
      if (!document || !document->findBody(body->body.bodyId))
        return std::nullopt;
      return ComponentWorkspaceBodyKey{
          {body->body.component.definitionId,
           body->body.component.occurrencePath},
          body->body.bodyId};
    };

    if (componentDefinitionEditContext_) {
      const auto treeBody = selectedTreeBody();
      const ComponentWorkspaceInstanceKey active{
          componentDefinitionEditContext_->targetDefinitionId,
          componentDefinitionEditContext_->occurrencePath};
      if (treeBody && treeBody->instance == active) return treeBody;

      const auto* item = featureTree_ ? featureTree_->currentItem() : nullptr;
      if (!item || item->data(0, Qt::UserRole).toInt() != 3)
        return std::nullopt;
      const BodyId id{
          item->data(0, Qt::UserRole + 2).toULongLong()};
      const Document& document = activePartDocument();
      if (id == kInvalidBodyId || !document.findBody(id)) return std::nullopt;
      return ComponentWorkspaceBodyKey{active, id};
    }
    if (standalonePartProjectActive()) {
      const auto* item = featureTree_ ? featureTree_->currentItem() : nullptr;
      if (!item || item->data(0, Qt::UserRole).toInt() != 3)
        return std::nullopt;
      const BodyId id{
          item->data(0, Qt::UserRole + 2).toULongLong()};
      if (id == kInvalidBodyId || !productDocument_.rootDocument().findBody(id))
        return std::nullopt;
      return ComponentWorkspaceBodyKey{
          {productDocument_.rootDefinitionId(), std::nullopt}, id};
    }
    return componentProductActive() ? selectedTreeBody() : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<ComponentExtractionTransaction>
MainWindow::prepareSelectedComponentExtraction() const noexcept {
  const auto body = selectedComponentExtractionBody();
  return body ? prepareComponentExtraction(*body) : std::nullopt;
}

std::optional<ComponentExtractionTransaction>
MainWindow::prepareComponentExtraction(
    const ComponentWorkspaceBodyKey& body) const noexcept {
  const auto childDefinition = lowestFreeComponentDefinitionId(productDocument_);
  const auto occurrence = lowestFreeComponentOccurrenceId(productDocument_);
  if (!childDefinition || !occurrence ||
      body.instance.definitionId == kInvalidDefinitionId ||
      body.bodyId == kInvalidBodyId)
    return std::nullopt;
  const Document* document = nullptr;
  if (body.instance.definitionId == productDocument_.rootDefinitionId()) {
    if (const auto* rootPart = productDocument_.rootPartDefinition())
      document = &rootPart->document();
  }
  if (!document)
    document = productDocument_.componentDocument(body.instance.definitionId);
  if (!document || !document->findBody(body.bodyId)) return std::nullopt;
  const ComponentExtractionAddress address{
      productDocument_.incarnation(), productDocument_.rootDefinitionId(),
      body.instance.definitionId,
      productDocument_.definitionStateId(body.instance.definitionId),
      body.bodyId, *childDefinition, *occurrence};
  auto prepared = productDocument_.prepareComponentExtraction(address);
  if (!prepared) return std::nullopt;
  return std::move(prepared.transaction);
}

void MainWindow::updateComponentHomePlacementAvailability() noexcept {
  if (!modelRibbon_) return;
  bool adoptEnabled = false;
  bool restoreEnabled = false;
  try {
    const bool quiescent =
        (componentProductActive() || rootComponentAuthoringActive()) &&
        isWritableComponentFormat(projectPersistenceState_.format) &&
        partDesignCoordinator_.activeTool() == PartDesignToolKind::None &&
        !modelHistory_.isApplying() &&
        QApplication::activeModalWidget() == nullptr && workspaceStack_ &&
        workspaceStack_->currentWidget() == viewport_ && viewport_ &&
        viewport_->componentSession() &&
        !viewport_->componentPlacementPreview() &&
        !viewport_->componentPlacementDragArmed();
    if (quiescent) {
      restoreEnabled = static_cast<bool>(
          productDocument_.prepareAllComponentHomePlacements());
      const auto& selected = viewport_->selectedComponentView();
      if (selected && selected->component().occurrencePath &&
          selected->isCurrentFor(
              viewport_->componentSession()->publicationHandle())) {
        const auto resolved = resolveComponentPlacementTarget(
            productDocument_, selected->component());
        if (resolved) {
          const auto request = [&](ComponentHomePlacementOperation operation) {
            return ComponentHomePlacementRequest{
                productDocument_.incarnation(),
                productDocument_.rootDefinitionId(),
                *selected->component().occurrencePath,
                resolved->targetDefinitionId,
                resolved->ownerDefinitionId,
                resolved->ownerStateId,
                resolved->placement,
                resolved->homePlacement,
                operation};
          };
          adoptEnabled = static_cast<bool>(
              productDocument_.prepareComponentHomePlacement(
                  request(ComponentHomePlacementOperation::AdoptCurrent)));
        }
      }
    }
  } catch (...) {
    adoptEnabled = false;
    restoreEnabled = false;
  }
  modelRibbon_->setComponentHomePlacementEnabled(adoptEnabled,
                                                 restoreEnabled);
}

void MainWindow::applyComponentHomePlacement(
    ComponentHomePlacementOperation operation) {
  updateComponentHomePlacementAvailability();
  if (!viewport_ || !viewport_->componentSession() ||
      modelHistory_.isApplying() || QApplication::activeModalWidget() ||
      partDesignCoordinator_.activeTool() != PartDesignToolKind::None)
    return;
  const auto& selected = viewport_->selectedComponentView();
  std::vector<ComponentHomePlacementTransaction> transactions;
  if (operation == ComponentHomePlacementOperation::Restore) {
    auto prepared = productDocument_.prepareAllComponentHomePlacements();
    if (!prepared) {
      if (prepared.status != ComponentHomePlacementStatus::NoChange)
        statusBar()->showMessage(
            componentHomePlacementDiagnostic(prepared.status), 4000);
      updateComponentHomePlacementAvailability();
      return;
    }
    transactions = std::move(prepared.transactions);
  } else {
    if (!selected || !selected->component().occurrencePath ||
        !selected->isCurrentFor(
            viewport_->componentSession()->publicationHandle()))
      return;
    const auto resolved = resolveComponentPlacementTarget(
        productDocument_, selected->component());
    if (!resolved) return;
    ComponentHomePlacementRequest request{
        productDocument_.incarnation(), productDocument_.rootDefinitionId(),
        *selected->component().occurrencePath, resolved->targetDefinitionId,
        resolved->ownerDefinitionId, resolved->ownerStateId,
        resolved->placement, resolved->homePlacement, operation};
    auto prepared = productDocument_.prepareComponentHomePlacement(request);
    if (!prepared) {
      if (prepared.status != ComponentHomePlacementStatus::NoChange)
        statusBar()->showMessage(
            componentHomePlacementDiagnostic(prepared.status), 4000);
      updateComponentHomePlacementAvailability();
      return;
    }
    transactions.push_back(*prepared.transaction);
  }
  try {
    auto projection = captureComponentWorkspaceProjection(productDocument_);
    if (!projection) return;
    ComponentWorkspaceInstanceKey instance{
        productDocument_.rootDefinitionId(), std::nullopt};
    if (selected && selected->isCurrentFor(
                        viewport_->componentSession()->publicationHandle()))
      instance = {selected->component().definitionId,
                  selected->component().occurrencePath};
    ComponentWorkspaceSemanticState state{
        instance, instance, std::nullopt, "component home placement"};
    std::optional<ComponentWorkspaceInstanceKey> hovered;
    if (viewport_->componentSession()->hoveredComponent()) {
      const auto& address =
          viewport_->componentSession()->hoveredComponent()->component();
      hovered = ComponentWorkspaceInstanceKey{address.definitionId,
                                              address.occurrencePath};
    }
    const project::ProjectPersistenceState afterPersistence{
        project::ProjectPersistenceFormat::V12};
    ComponentHomePlacementColdCommand command{
        transactions.front(),
        state,
        state,
        projection->instances,
        projection->instances,
        projection->bodies,
        projection->bodies,
        projectPersistenceState_,
        afterPersistence};
    command.additionalTransactions.assign(transactions.begin() + 1,
                                          transactions.end());
    if (modelHistory_.prepareComponentHomePlacement(productDocument_,
                                                     command) !=
        ModelCommandHistoryStatus::Prepared) {
      statusBar()->showMessage(
          QString::fromUtf8(
              "Не удалось подготовить изменение исходного положения"),
          4000);
      return;
    }
    if (componentPresentationEpochClock_ ==
        std::numeric_limits<std::uint64_t>::max()) {
      modelHistory_.discardPrepared();
      return;
    }
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    ModelWorkspaceProjectionSuccessor successor(
        ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
        profile, state, componentExtractionProjectionFailurePoint_, hovered);
    const auto applied =
        transactions.size() == 1
            ? productDocument_.applyComponentHomePlacement(
                  transactions.front(), successor)
            : productDocument_.applyAllComponentHomePlacements(transactions,
                                                               successor);
    if (applied != ComponentHomePlacementStatus::Committed) {
      modelHistory_.discardPrepared();
      statusBar()->showMessage(componentHomePlacementDiagnostic(applied), 4000);
      updateComponentHomePlacementAvailability();
      return;
    }
    if (modelHistory_.commitPrepared() !=
        ModelCommandHistoryStatus::Committed)
      std::terminate();
    if (auto component = successor.takeComponent()) {
      QPointer<MainWindow> receiver(this);
      if (rootComponentAuthoringActive())
        installActiveComponentProjection(std::move(component));
      else
        installPreparedComponentProjection(std::move(component));
      if (!receiver) return;
    }
    projectPersistenceState_ = afterPersistence;
    applyProjectKind(EditorProjectKind::Part);
    setWindowModified(!modelHistory_.isAtSavepoint());
    updateUndoAvailability();
    updateCreateComponentFromBodyAvailability();
    statusBar()->showMessage(
        operation == ComponentHomePlacementOperation::AdoptCurrent
            ? QString::fromUtf8("Текущее положение сохранено как исходное")
            : QString::fromUtf8(
                  "Все компоненты возвращены в исходное положение"),
        3000);
  } catch (...) {
    modelHistory_.discardPrepared();
    statusBar()->showMessage(
        QString::fromUtf8(
            "Не удалось изменить исходное положение: недостаточно ресурсов"),
        4000);
  }
}

void MainWindow::updateCreateComponentFromBodyAvailability() noexcept {
  if (!modelRibbon_) return;
  updateComponentHomePlacementAvailability();
  const bool quiescent =
      projectKind_ == EditorProjectKind::Part &&
      !definitionEditContext_.has_value() &&
      partDesignCoordinator_.activeTool() == PartDesignToolKind::None &&
      !modelHistory_.isApplying() && QApplication::activeModalWidget() == nullptr &&
      workspaceStack_ && workspaceStack_->currentWidget() == viewport_;
  const auto selectedBody = selectedComponentExtractionBody();
  const auto prepared = quiescent ? prepareSelectedComponentExtraction()
                                  : std::nullopt;
  modelRibbon_->setCreateComponentFromBodyEnabled(prepared.has_value());
  auto* button = modelRibbon_->findChild<QToolButton*>(
      QStringLiteral("createComponentFromBodyCommand"));
  if (!button) return;
  if (prepared) {
    const auto frozenCount = prepared->plan.frozenSketchSupports.size();
    button->setToolTip(
        frozenCount == 0
            ? QString::fromUtf8(
                  "Перенести выбранное тело в новый вложенный компонент")
            : QString::fromUtf8(
                  "Перенести выбранное тело в новый вложенный компонент; "
                  "внешняя опора эскиза будет зафиксирована"));
    return;
  }
  if (quiescent && selectedBody) {
    const Document* document = productDocument_.rootPartDefinition()
                                   ? &productDocument_.rootPartDefinition()
                                          ->document()
                                   : productDocument_.componentDocument(
                                         selectedBody->instance.definitionId);
    if (document) {
      const auto plan =
          document->planComponentExtraction(selectedBody->bodyId);
      if (plan.status ==
          ComponentExtractionPlanStatus::CrossDefinitionDependency) {
        button->setToolTip(componentExtractionDiagnostic(
            ComponentExtractionStatus::CrossDefinitionDependency,
            plan.diagnostic));
        return;
      }
    }
  }
  button->setToolTip(QString::fromUtf8(
      "Выберите тело активного компонента для создания вложенного компонента"));
}

void MainWindow::createComponentFromSelectedBody() {
  if (partDesignCoordinator_.activeTool() != PartDesignToolKind::None ||
      modelHistory_.isApplying() || QApplication::activeModalWidget() ||
      !workspaceStack_ || workspaceStack_->currentWidget() != viewport_)
    return;
  const auto selectedBody = selectedComponentExtractionBody();
  if (!selectedBody) return;
  createComponentFromBody(*selectedBody);
}

void MainWindow::createComponentFromBody(
    ComponentWorkspaceBodyKey selectedBody) {
  if (partDesignCoordinator_.activeTool() != PartDesignToolKind::None ||
      modelHistory_.isApplying() || QApplication::activeModalWidget() ||
      !workspaceStack_ || workspaceStack_->currentWidget() != viewport_)
    return;
  const Document* selectedDocument = nullptr;
  if (selectedBody.instance.definitionId ==
      productDocument_.rootDefinitionId()) {
    if (const auto* rootPart = productDocument_.rootPartDefinition())
      selectedDocument = &rootPart->document();
  }
  if (!selectedDocument)
    selectedDocument = productDocument_.componentDocument(
        selectedBody.instance.definitionId);
  if (!selectedDocument || !selectedDocument->findBody(selectedBody.bodyId))
    return;
  std::optional<ComponentWorkspaceInstanceKey> activeBefore;
  if (componentDefinitionEditContext_) {
    activeBefore = ComponentWorkspaceInstanceKey{
        componentDefinitionEditContext_->targetDefinitionId,
        componentDefinitionEditContext_->occurrencePath};
  } else if (standalonePartProjectActive() ||
             productDocument_.rootComponentDefinition()) {
    // A standalone part is the future root component; a component product with
    // no explicit edit context also falls back to its root. In both cases keep
    // root authoring active instead of activating the newly created child.
    activeBefore = ComponentWorkspaceInstanceKey{
        productDocument_.rootDefinitionId(), std::nullopt};
  }
  if (componentDefinitionEditContext_) {
    leaveComponentDefinitionEdit();
    if (componentDefinitionEditContext_) return;
  }
  const auto childDefinition = lowestFreeComponentDefinitionId(productDocument_);
  const auto occurrence = lowestFreeComponentOccurrenceId(productDocument_);
  if (!childDefinition || !occurrence) return;
  auto prepared = productDocument_.prepareComponentExtraction(
      {productDocument_.incarnation(), productDocument_.rootDefinitionId(),
       selectedBody.instance.definitionId,
       productDocument_.definitionStateId(
           selectedBody.instance.definitionId),
       selectedBody.bodyId, *childDefinition, *occurrence});
  if (!prepared) {
    statusBar()->showMessage(componentExtractionDiagnostic(prepared.status),
                             5000);
    return;
  }
  const std::size_t frozenSupportCount =
      prepared.transaction->plan.frozenSketchSupports.size();
  try {
    auto beforeProjection =
        captureComponentWorkspaceProjection(productDocument_);
    if (!beforeProjection) return;
    auto afterProjection = *beforeProjection;
    std::vector<std::pair<ComponentWorkspaceInstanceKey,
                          ComponentWorkspaceInstanceKey>>
        createdInstances;
    for (const auto& candidate : beforeProjection->instances) {
      if (candidate.definitionId != selectedBody.instance.definitionId)
        continue;
      std::vector<OccurrenceId> entries;
      if (candidate.path) entries = candidate.path->entries();
      entries.push_back(*occurrence);
      auto path = OccurrencePath::create(std::move(entries));
      if (!path) return;
      createdInstances.emplace_back(
          candidate,
          ComponentWorkspaceInstanceKey{*childDefinition,
                                        std::move(*path.path)});
    }
    if (createdInstances.empty()) return;
    const auto activeCreated = std::find_if(
        createdInstances.begin(), createdInstances.end(),
        [&selectedBody](const auto& pair) {
          return pair.first == selectedBody.instance;
        });
    if (activeCreated == createdInstances.end()) return;
    const ComponentWorkspaceBodyKey movedBefore = selectedBody;
    for (auto& body : afterProjection.bodies) {
      if (body.bodyId != selectedBody.bodyId) continue;
      const auto created = std::find_if(
          createdInstances.begin(), createdInstances.end(),
          [&body](const auto& pair) { return pair.first == body.instance; });
      if (created != createdInstances.end()) body.instance = created->second;
    }
    for (auto& drawing : afterProjection.drawings) {
      const bool moves = std::any_of(
          prepared.transaction->createdChildDrawings.begin(),
          prepared.transaction->createdChildDrawings.end(),
          [&drawing](const ComponentDrawing& moved) {
            return moved.id() == drawing.drawingId;
          });
      if (!moves) continue;
      const auto created = std::find_if(
          createdInstances.begin(), createdInstances.end(),
          [&drawing](const auto& pair) {
            return pair.first == drawing.instance;
          });
      if (created != createdInstances.end())
        drawing.instance = created->second;
    }
    for (const auto& pair : createdInstances)
      afterProjection.instances.push_back(pair.second);
    canonicalizeComponentWorkspaceProjection(afterProjection);

    ComponentWorkspaceSemanticState beforeState{
        selectedBody.instance, selectedBody.instance, movedBefore,
        "before component extraction"};
    ComponentWorkspaceSemanticState afterState{
        selectedBody.instance, selectedBody.instance, std::nullopt,
        "after component extraction"};
    const auto afterPersistence = componentPersistenceForSave(
        productDocument_, projectPersistenceState_);
    ComponentExtractionColdCommand command{
        *prepared.transaction,
        beforeState,
        afterState,
        std::move(beforeProjection->instances),
        std::move(afterProjection.instances),
        std::move(beforeProjection->bodies),
        std::move(afterProjection.bodies),
        projectPersistenceState_,
        afterPersistence,
        std::move(beforeProjection->drawings),
        std::move(afterProjection.drawings)};
    if (modelHistory_.prepareComponentExtraction(productDocument_, command) !=
        ModelCommandHistoryStatus::Prepared) {
      statusBar()->showMessage(
          QString::fromUtf8("Не удалось подготовить команду создания компонента"),
          5000);
      return;
    }
    if (componentPresentationEpochClock_ ==
        std::numeric_limits<std::uint64_t>::max()) {
      modelHistory_.discardPrepared();
      return;
    }
    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    ModelWorkspaceProjectionSuccessor successor(
        ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
        profile, afterState, componentExtractionProjectionFailurePoint_);
    const auto applied = productDocument_.applyComponentExtraction(
        *prepared.transaction, successor, componentExtractionFailurePoint_);
    if (applied != ComponentExtractionStatus::Committed) {
      modelHistory_.discardPrepared();
      statusBar()->showMessage(componentExtractionDiagnostic(
                                   applied,
                                   prepared.transaction->plan.diagnostic),
                               5000);
      updateCreateComponentFromBodyAvailability();
      return;
    }
    if (modelHistory_.commitPrepared() !=
        ModelCommandHistoryStatus::Committed)
      std::terminate();
    componentModelHistories_.clear();
    auto projection = successor.takeComponent();
    if (!projection) std::terminate();
    projectPersistenceState_ = afterPersistence;
    installPreparedComponentProjection(std::move(projection));
    applyProjectKind(EditorProjectKind::Part);
    setWindowModified(!modelHistory_.isAtSavepoint());
    if (componentTreeModel_ && activeBefore) {
      const auto findOwnerComponent = [&](const auto& self,
                                          const QModelIndex& parent)
          -> QModelIndex {
        for (int row = 0; row < componentTreeModel_->rowCount(parent); ++row) {
          const QModelIndex candidate =
              componentTreeModel_->index(row, 0, parent);
          const auto address = componentTreeModel_->addressForIndex(candidate);
          const auto* occurrenceAddress =
              address ? std::get_if<ComponentTreeOccurrenceAddress>(
                            &address->get().value())
                      : nullptr;
          const auto* rootAddress =
              address ? std::get_if<ComponentTreeRootAddress>(
                            &address->get().value())
                      : nullptr;
          if ((rootAddress &&
               rootAddress->instance.definitionId ==
                   activeBefore->definitionId &&
               rootAddress->instance.occurrencePath ==
                   activeBefore->path) ||
              (occurrenceAddress &&
               occurrenceAddress->instance.definitionId ==
                   activeBefore->definitionId &&
               occurrenceAddress->instance.occurrencePath ==
                   activeBefore->path))
            return candidate;
          const QModelIndex nested = self(self, candidate);
          if (nested.isValid()) return nested;
        }
        return {};
      };
      const QModelIndex ownerComponent =
          findOwnerComponent(findOwnerComponent, QModelIndex{});
      if (ownerComponent.isValid())
        enterComponentDefinitionEdit(ownerComponent);
    }
    updateUndoAvailability();
    updateCreateComponentFromBodyAvailability();
    statusBar()->showMessage(
        frozenSupportCount == 0
            ? QString::fromUtf8("Компонент создан")
            : QString::fromUtf8(
                  "Компонент создан; внешняя опора эскиза зафиксирована"),
        4000);
  } catch (...) {
    modelHistory_.discardPrepared();
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось создать компонент: недостаточно ресурсов"),
        5000);
  }
}

void MainWindow::commitComponentPlacementDrag(
    ComponentPlacementDragRequest request) {
  if ((!componentProductActive() && !rootComponentAuthoringActive()) ||
      !isWritableComponentFormat(projectPersistenceState_.format) ||
      partDesignCoordinator_.activeTool() != PartDesignToolKind::None ||
      modelHistory_.isApplying() || QApplication::activeModalWidget() ||
      !workspaceStack_ || workspaceStack_->currentWidget() != viewport_ ||
      !viewport_ || !viewport_->componentSession() ||
      componentPresentationEpochClock_ ==
          std::numeric_limits<std::uint64_t>::max() ||
      !request.publication.isSameCurrentPublication(
          viewport_->componentSession()->publicationHandle()) ||
      request.component.rootDefinitionId !=
          productDocument_.rootDefinitionId() ||
      !request.component.occurrencePath)
    return;

  const auto requestSnapshot = request.publication.snapshotIfCurrent();
  if (!requestSnapshot || request.component.epoch != requestSnapshot->epoch() ||
      requestSnapshot->treeProjection().productIncarnation !=
          productDocument_.incarnation() ||
      requestSnapshot->treeProjection().productIdentity !=
          productDocument_.identity() ||
      requestSnapshot->treeProjection().rootDefinitionId !=
          productDocument_.rootDefinitionId())
    return;

  const auto& selected = viewport_->selectedComponentView();
  if (!selected || selected->component() != request.component) return;
  const auto resolved =
      resolveComponentPlacementTarget(productDocument_, request.component);
  if (!resolved ||
      resolved->targetDefinitionId != request.component.definitionId)
    return;

  try {
    const ComponentPlacementRequest modelRequest{
        productDocument_.incarnation(), productDocument_.rootDefinitionId(),
        request.component.occurrencePath, resolved->targetDefinitionId,
        resolved->ownerStateId, resolved->placement, request.worldDeltaMm};
    auto prepared = productDocument_.prepareComponentPlacement(modelRequest);
    if (!prepared) {
      if (prepared.status != ComponentPlacementStatus::NoChange)
        statusBar()->showMessage(componentPlacementDiagnostic(prepared.status),
                                 4000);
      return;
    }

    auto projection = captureComponentWorkspaceProjection(productDocument_);
    if (!projection) return;
    const ComponentWorkspaceInstanceKey instance{
        request.component.definitionId,
        request.component.occurrencePath};
    ComponentWorkspaceSemanticState beforeState{
        instance, instance, std::nullopt, "before component placement"};
    ComponentWorkspaceSemanticState afterState{
        instance, instance, std::nullopt, "after component placement"};
    std::optional<ComponentWorkspaceInstanceKey> hoveredInstance;
    if (const auto* session = viewport_->componentSession();
        session && session->hoveredComponent()) {
      const auto& hovered = session->hoveredComponent()->component();
      hoveredInstance = ComponentWorkspaceInstanceKey{
          hovered.definitionId, hovered.occurrencePath};
    }
    const project::ProjectPersistenceState afterPersistence{
        project::ProjectPersistenceFormat::V12};
    ComponentPlacementColdCommand command{
        *prepared.transaction,
        beforeState,
        afterState,
        projection->instances,
        projection->instances,
        projection->bodies,
        projection->bodies,
        projectPersistenceState_,
        afterPersistence};
    if (modelHistory_.prepareComponentPlacement(productDocument_, command) !=
        ModelCommandHistoryStatus::Prepared) {
      statusBar()->showMessage(
          QString::fromUtf8("Не удалось подготовить перемещение компонента"),
          4000);
      return;
    }

    const AssemblyMeshProfile profile{
        viewport_->meshQuality() == ViewportMeshQuality::High
            ? AssemblyMeshQuality::High
            : AssemblyMeshQuality::Normal};
    ModelWorkspaceProjectionSuccessor successor(
        ProductPresentationEpoch{componentPresentationEpochClock_ + 1},
        profile, afterState, componentExtractionProjectionFailurePoint_,
        hoveredInstance);
    const auto applied = productDocument_.applyComponentPlacement(
        *prepared.transaction, successor);
    if (applied != ComponentPlacementStatus::Committed) {
      modelHistory_.discardPrepared();
      statusBar()->showMessage(componentPlacementDiagnostic(applied), 4000);
      return;
    }
    if (modelHistory_.commitPrepared() !=
        ModelCommandHistoryStatus::Committed)
      std::terminate();
    auto preparedProjection = successor.takeComponent();
    if (!preparedProjection) std::terminate();

    QPointer<MainWindow> receiver(this);
    if (rootComponentAuthoringActive())
      installActiveComponentProjection(std::move(preparedProjection));
    else
      installPreparedComponentProjection(std::move(preparedProjection));
    if (!receiver) return;
    receiver->projectPersistenceState_ = afterPersistence;
    receiver->applyProjectKind(EditorProjectKind::Part);
    receiver->setWindowModified(!receiver->modelHistory_.isAtSavepoint());
    receiver->updateUndoAvailability();
    receiver->updateCreateComponentFromBodyAvailability();
    receiver->statusBar()->showMessage(
        QString::fromUtf8("Компонент перемещён"), 2500);
  } catch (...) {
    modelHistory_.discardPrepared();
    statusBar()->showMessage(
        QString::fromUtf8(
            "Не удалось переместить компонент: недостаточно ресурсов"),
        4000);
  }
}

void MainWindow::commitAssemblyReplacement(
    std::unique_ptr<PreparedAssemblyWorkspace> prepared,
    const QString& path) noexcept {
  if (!prepared || !prepared->epoch().isValid()) return;

  clearComponentDrawingWorkspace();

  // Nothing below performs mesh/topology construction, opens a modal loop or
  // processes events. All fallible assembly work completed in the candidate.
  cancelAssemblyInsertBeforeReplacement();
  preparePartDesignForReplacement();
  detachDefinitionEditForReplacement();
  detachAssemblyInsertUi(true);
  clearAssemblyCommandHistoryForReplacement();
  if (modelRibbon_) modelRibbon_->clearActiveTool();

  finishEditUiTransaction();
  mirrorPresentationBefore_.reset();
  historicalLegacyExtrusionSourceSketchId_.reset();
  selectedExtrusionSource_.reset();
  currentSketchFaceReference_.reset();
  selectedReferenceImageId_ = kInvalidReferenceImageId;
  referenceImageEditBefore_.reset();
  referenceImageEditSelectionBefore_.reset();
  pendingReferenceImagePath_.clear();
  pendingReferenceImagePixelSize_ = {};
  sketchViews_.clear();
  editingSketchIndex_.reset();

  QObject::disconnect(assemblyTreeSelectionConnection_);
  assemblyTreeSelectionConnection_ = {};
  QObject::disconnect(componentTreeSelectionConnection_);
  componentTreeSelectionConnection_ = {};
  if (assemblyTreeView_) assemblyTreeView_->setModel(nullptr);
  if (componentTreeView_) componentTreeView_->setModel(nullptr);
  if (assemblyTreeModel_) {
    delete assemblyTreeModel_;
    assemblyTreeModel_ = nullptr;
  }
  delete componentTreeModel_;
  componentTreeModel_ = nullptr;
  viewport_->resetScene();
  modelHistory_.clear(true);
  if (historyLayout_) {
    while (QLayoutItem* item = historyLayout_->takeAt(0)) {
      delete item->widget();
      delete item;
    }
  }
  historySteps_.clear();
  historyPosition_ = 0;
  if (historyTimeline_) {
    const QSignalBlocker blocker(historyTimeline_);
    historyTimeline_->setStepCount(0);
    historyTimeline_->setPosition(0);
  }
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::OldWorkspaceDetached, false);

  productDocument_ = prepared->releaseProduct();
  linkedPartRuntimeStates_ =
      std::move(prepared->linkedPartRuntimeStates);
  mateRuntimeDiagnostics_ =
      std::move(prepared->mateRuntimeDiagnostics);
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::ProductReplaced, false);
  // The model root has changed. Establish the fail-closed semantic mode before
  // any QObject-visible state can emit and synchronously re-enter callbacks.
  projectKind_ = EditorProjectKind::AssemblyReadOnly;
  if (prepared->persistenceState)
    projectPersistenceState_ = *prepared->persistenceState;
  mateDiagnosticsValidityStamp_ =
      projectPersistenceState_.format == project::ProjectPersistenceFormat::V9
          ? captureMateDiagnosticsValidityStamp()
          : std::nullopt;
  auto session = prepared->releaseSession();
  auto tree = prepared->releaseTreeModel();
  assemblyPresentationEpochClock_ = prepared->epoch().value();

  assemblyTreeModel_ = tree.release();
  assemblyTreeModel_->setParent(assemblyTreeView_);
  assemblyTreeView_->setModel(assemblyTreeModel_);
  assemblyTreeView_->expandAll();
  assemblyTreeSelectionConnection_ = connect(
      assemblyTreeView_->selectionModel(),
      &QItemSelectionModel::currentChanged, this,
      [this](const QModelIndex& current, const QModelIndex&) {
        selectAssemblyTreeIndex(current);
      });
  viewport_->installAssemblySession(std::move(session));
  workspaceStack_->setCurrentWidget(viewport_);
  applyProjectKind(EditorProjectKind::AssemblyReadOnly);
  setProjectPath(path);
  setWindowModified(false);
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::WorkspaceInstalled, false);
  statusBar()->showMessage(
      QString::fromUtf8("Сборка открыта"), 4000);
}

void MainWindow::commitDocumentReplacement(
    StagedApplicationDocument staged, DocumentReplacementOrigin origin,
    const QString& pathOrLabel) {
  // Assembly activation has a distinct prepared-workspace commit path. Keep
  // this part-only guard before generation invalidation, tool cancellation,
  // history clearing or any other mutation so an internal caller fails closed.
  if (staged.rootDefinitionKind() != DefinitionKind::Part ||
      staged.rootPartDocument() == nullptr)
    return;

  clearComponentDrawingWorkspace();

  const bool opened = origin == DocumentReplacementOrigin::OpenProject;
  const auto persistenceState = staged.persistenceState;
  const auto historicalLegacyExtrusionSourceSketchId =
      staged.historicalLegacyExtrusionSourceSketchId;

  // This is the sole commit protocol for a completely staged replacement.
  // Invalidate queued work and cancel every topology-owning session while the
  // old Document is alive. Failed staging never enters this method.
  cancelAssemblyInsertBeforeReplacement();
  preparePartDesignForReplacement();
  detachDefinitionEditForReplacement();
  detachAssemblyInsertUi(true);
  clearAssemblyCommandHistoryForReplacement();
  if (modelRibbon_) modelRibbon_->clearActiveTool();

  // Everything below may carry IDs or topology owned by the outgoing
  // Document. Discard it, and detach the viewport, before destroying that
  // Document. Queued callbacks are generation-guarded by the invalidation
  // above.
  finishEditUiTransaction();
  mirrorPresentationBefore_.reset();
  historicalLegacyExtrusionSourceSketchId_.reset();
  selectedExtrusionSource_.reset();
  currentSketchFaceReference_.reset();
  selectedReferenceImageId_ = kInvalidReferenceImageId;
  referenceImageEditBefore_.reset();
  referenceImageEditSelectionBefore_.reset();
  pendingReferenceImagePath_.clear();
  pendingReferenceImagePixelSize_ = {};
  sketchViews_.clear();
  editingSketchIndex_.reset();
  viewport_->resetScene();
  QObject::disconnect(assemblyTreeSelectionConnection_);
  assemblyTreeSelectionConnection_ = {};
  QObject::disconnect(componentTreeSelectionConnection_);
  componentTreeSelectionConnection_ = {};
  if (assemblyTreeView_) assemblyTreeView_->setModel(nullptr);
  if (componentTreeView_) componentTreeView_->setModel(nullptr);
  if (assemblyTreeModel_) {
    delete assemblyTreeModel_;
    assemblyTreeModel_ = nullptr;
  }
  delete componentTreeModel_;
  componentTreeModel_ = nullptr;

  // History deltas may own cold model slices from the outgoing root. Release
  // them at the hard boundary, after all observers have detached but before
  // the old ProductDocument is destroyed.
  modelHistory_.clear(opened);
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::OldWorkspaceDetached, false);

  // The staged carrier is the only replacement commit point. Its root
  // Document allocation moves intact; every later use reacquires the root.
  productDocument_ = std::move(staged).releaseProductDocument();
  linkedPartRuntimeStates_.clear();
  mateRuntimeDiagnostics_.clear();
  mateDiagnosticsValidityStamp_.reset();
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::ProductReplaced, false);
  if (persistenceState)
    projectPersistenceState_ = *persistenceState;
  // No events are processed during commit. Set the explicit mode before the
  // guarded part-only rebuild helpers restore the new part workspace.
  applyProjectKind(EditorProjectKind::Part);
  historicalLegacyExtrusionSourceSketchId_ =
      historicalLegacyExtrusionSourceSketchId;
  mutableStandaloneDocument().reserveIdsForEditing();
  currentSketchSupport_ = QStringLiteral("XY");
  currentSketchPlacement_ = SketchPlacement::xy();
  viewport_->setBox(activePartDocument().box());
  for (const auto& item : activePartDocument().sketches()) {
    sketchViews_.push_back({item.id});
    viewport_->addSketch(item.id, item.geometry,
                         sketchPresentationLabel(item), item.placement);
  }
  extrudeOperationManuallyChanged_ = false;
  if (opened) sketchCanvas_->resetSketch();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  if (opened) applyHistoryPosition(historyPosition_);
  refreshBodyViewFromDocument();
  workspaceStack_->setCurrentWidget(viewport_);
  if (opened) {
    setProjectPath(pathOrLabel);
    setWindowModified(false);
  } else {
    viewport_->fitAll();
    setWindowModified(true);
  }
  traceAssemblyReplacementStep(
      AssemblyInsertTeardownStep::WorkspaceInstalled, false);
  updateUndoAvailability();
  updateUpgradeProjectFormatAction();
  if (!opened)
    statusBar()->showMessage(
        QString::fromUtf8("STEP импортирован: ") + pathOrLabel, 5000);
}

void MainWindow::exportStep() {
  if (!standalonePartProjectActive()) return;
  QString fileName = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Экспортировать STEP"),
      windowFilePath().isEmpty()
          ? QStringLiteral("Модель.step")
          : QFileInfo(windowFilePath()).absolutePath() + QLatin1Char('/') +
                QFileInfo(windowFilePath()).completeBaseName() +
                QStringLiteral(".step"),
      QStringLiteral("STEP files (*.step *.stp)"));
  if (fileName.isEmpty()) return;
  if (!fileName.endsWith(QStringLiteral(".step"), Qt::CaseInsensitive) &&
      !fileName.endsWith(QStringLiteral(".stp"), Qt::CaseInsensitive))
    fileName += QStringLiteral(".step");

  const auto exported = projectApplicationService_.exportStep(fileName,
                                                               activePartDocument());
  if (!exported.succeeded()) {
    QMessageBox::critical(
        this, QString::fromUtf8("Не удалось экспортировать STEP"),
        exported.detail);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("STEP сохранён: ") + fileName,
                           5000);
}

void MainWindow::exportStl() {
  if (!standalonePartProjectActive()) return;
  const auto preflight =
      projectApplicationService_.preflightStlExport(activePartDocument());
  if (!preflight.succeeded()) {
    QMessageBox::information(
        this, QString::fromUtf8("Экспорт STL"),
        preflight.detail.isEmpty()
            ? QString::fromUtf8(
                  "Документ не содержит корректного B-Rep тела для экспорта.")
            : preflight.detail);
    return;
  }
  QString fileName = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Экспортировать STL"),
      windowFilePath().isEmpty()
          ? QStringLiteral("Модель.stl")
          : QFileInfo(windowFilePath()).absolutePath() + QLatin1Char('/') +
                QFileInfo(windowFilePath()).completeBaseName() +
                QStringLiteral(".stl"),
      QStringLiteral("STL (*.stl)"));
  if (fileName.isEmpty()) return;
  if (!fileName.endsWith(QStringLiteral(".stl"), Qt::CaseInsensitive))
    fileName += QStringLiteral(".stl");

  const auto exported = projectApplicationService_.exportStl(fileName,
                                                              activePartDocument());
  if (!exported.succeeded()) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка экспорта STL"),
                          exported.detail);
    return;
  }
  statusBar()->showMessage(QString::fromUtf8("STL сохранён: ") + fileName, 5000);
}

void MainWindow::setProjectPath(const QString& path) {
  setWindowFilePath(path);
  setWindowTitle(QFileInfo(path).completeBaseName() +
                 QString::fromUtf8(" — СОЛИДАР3D [*]"));
  updateComponentTreePresentation();
  updateUpgradeProjectFormatAction();
}

void MainWindow::buildUi() {
  workspaceStack_ = new QStackedWidget(this);
  sketchCanvas_ = new SketchCanvas(workspaceStack_);

  connect(
      sketchCanvas_,
      &SketchCanvas::constraintStatusChanged,
      this,
      [this](const QString& status) {
        if (!partProjectActive()) return;
        statusBar()->showMessage(status);
      });

  viewport_ = new Viewport(workspaceStack_);
  viewport_->setObjectName(QStringLiteral("mainViewport"));
  drawingSheet_ = new DrawingSheetView(workspaceStack_);
  viewport_->setBox(activePartDocument().box());
  viewport_->setSketch(sketchCanvas_->sketch(), currentSketchPlacement_);
  drawingSheet_->setRectangle(activePartDocument().box().widthMm, activePartDocument().box().depthMm);
  workspaceStack_->addWidget(viewport_);
  workspaceStack_->addWidget(sketchCanvas_);
  workspaceStack_->addWidget(drawingSheet_);
  connect(drawingSheet_, &DrawingSheetView::backRequested, this,
          &MainWindow::closeComponentDrawing);
  setCentralWidget(workspaceStack_);

  extrusionDock_ = new QDockWidget(QString::fromUtf8("Выдавливание"), this);
  extrusionDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  extrusionDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* extrusionPanel = new QWidget(extrusionDock_);
  auto* extrusionPanelLayout = new QVBoxLayout(extrusionPanel);
  extrusionPanelLayout->setContentsMargins(16, 14, 16, 14);
  extrusionPanelLayout->setSpacing(12);
  const auto* extrudeHelp = partDesignToolHelp(PartDesignToolKind::Extrude);
  auto* extrusionDescription = new QLabel(extrudeHelp->shortDescription,
                                          extrusionPanel);
  extrusionDescription->setWordWrap(true);
  extrusionDescription->setProperty("uiRole", "secondaryText");
  auto* extrusionHint = new QLabel(QString::fromUtf8(
      "Потяните синюю стрелку в 3D-виде или введите точное значение."),
      extrusionPanel);
  extrusionHint->setWordWrap(true);
  extrusionHint->setProperty("uiRole", "secondaryText");
  auto* extrusionForm = new QFormLayout;
  extrusionLengthSpin_ = new QDoubleSpinBox(extrusionPanel);
  extrusionLengthSpin_->setRange(-100000.0, 100000.0);
  extrusionLengthSpin_->setDecimals(2);
  extrusionLengthSpin_->setSuffix(QStringLiteral(" mm"));
  extrusionLengthSpin_->setValue(activePartDocument().box().heightMm);
  extrusionForm->addRow(QString::fromUtf8("Длина:"), extrusionLengthSpin_);
  extrusionOperationCombo_ = new QComboBox(extrusionPanel);
  extrusionOperationCombo_->addItems(
      {QString::fromUtf8("Новое тело"), QString::fromUtf8("Объединить"),
       QString::fromUtf8("Вырезать")});
  extrusionForm->addRow(QString::fromUtf8("Операция:"),
                        extrusionOperationCombo_);
  extrusionReverseCheck_ =
      new QCheckBox(QString::fromUtf8("Обратное направление"), extrusionPanel);
  extrusionForm->addRow(QString::fromUtf8("Направление:"),
                        extrusionReverseCheck_);
  auto* extrusionButtons = new QHBoxLayout;
  auto* cancelExtrusion = new QPushButton(QString::fromUtf8("Отмена"), extrusionPanel);
  auto* acceptExtrusion = new QPushButton(QString::fromUtf8("Применить"), extrusionPanel);
  acceptExtrusion->setDefault(true);
  acceptExtrusion->setProperty("uiRole", "primaryAction");
  acceptExtrusion->setObjectName("primaryAction");
  extrusionButtons->addWidget(cancelExtrusion);
  extrusionButtons->addWidget(acceptExtrusion);
  extrusionPanelLayout->addWidget(extrusionDescription);
  extrusionPanelLayout->addWidget(extrusionHint);
  extrusionPanelLayout->addLayout(extrusionForm);
  extrusionPanelLayout->addStretch();
  extrusionPanelLayout->addLayout(extrusionButtons);
  extrusionDock_->setWidget(extrusionPanel);
  extrusionDock_->setObjectName(QStringLiteral("extrusionParametersDock"));
  extrusionDock_->setMinimumWidth(250);
  addDockWidget(Qt::RightDockWidgetArea, extrusionDock_);
  extrusionDock_->hide();

  revolveDock_ = new QDockWidget(QString::fromUtf8("Вращение"), this);
  revolveDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  revolveDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* revolvePanel = new QWidget(revolveDock_);
  auto* revolveLayout = new QVBoxLayout(revolvePanel);
  revolveLayout->setContentsMargins(16, 14, 16, 14);
  revolveLayout->setSpacing(12);
  const auto* revolveHelp = partDesignToolHelp(PartDesignToolKind::Revolve);
  auto* revolveTitle = new QLabel(revolveHelp->title.toUpper(), revolvePanel);
  QFont revolveTitleFont = revolveTitle->font();
  revolveTitleFont.setBold(true); revolveTitle->setFont(revolveTitleFont);
  auto* revolveDescription = new QLabel(revolveHelp->shortDescription,
                                        revolvePanel);
  revolveDescription->setWordWrap(true);
  revolveDescription->setProperty("uiRole", "secondaryText");
  revolveStepHint_ = new QLabel(revolveHelp->selectionHint, revolvePanel);
  revolveStepHint_->setObjectName("revolveStepHint");
  revolveStepHint_->setWordWrap(true);
  auto* revolveForm = new QFormLayout;
  revolveProfileCombo_ = new QComboBox(revolvePanel);
  revolveProfileSummary_ = new QLabel(
      QString::fromUtf8("Профиль не выбран"), revolvePanel);
  revolveProfileSummary_->setProperty("uiRole", "secondaryText");
  revolveProfileSummary_->setWordWrap(true);
  revolveAxisCombo_ = new QComboBox(revolvePanel);
  revolveAngleSpin_ = new QDoubleSpinBox(revolvePanel);
  revolveAngleSpin_->setRange(0.01, 360.0);
  revolveAngleSpin_->setDecimals(2);
  revolveAngleSpin_->setValue(360.0);
  revolveAngleSpin_->setSuffix(QString::fromUtf8(" °"));
  revolveOperationCombo_ = new QComboBox(revolvePanel);
  revolveOperationCombo_->addItems({QString::fromUtf8("Новое тело"),
                                    QString::fromUtf8("Объединить"),
                                    QString::fromUtf8("Вырезать")});
  revolveReverseCheck_ = new QCheckBox(
      QString::fromUtf8("Обратить направление"), revolvePanel);
  auto* profileRow = new QWidget(revolvePanel);
  auto* profileRowLayout = new QVBoxLayout(profileRow);
  profileRowLayout->setContentsMargins(0, 0, 0, 0);
  profileRowLayout->setSpacing(6);
  auto* profileControls = new QHBoxLayout;
  auto* reselectProfile = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), profileRow);
  profileControls->addWidget(revolveProfileCombo_);
  profileControls->addWidget(reselectProfile);
  profileRowLayout->addLayout(profileControls);
  profileRowLayout->addWidget(revolveProfileSummary_);
  auto* axisRow = new QWidget(revolvePanel);
  auto* axisRowLayout = new QHBoxLayout(axisRow);
  axisRowLayout->setContentsMargins(0, 0, 0, 0);
  auto* reselectAxis = new QPushButton(QString::fromUtf8("Выбрать в 3D"), axisRow);
  axisRowLayout->addWidget(revolveAxisCombo_);
  axisRowLayout->addWidget(reselectAxis);
  revolveForm->addRow(QString::fromUtf8("Профили:"), profileRow);
  revolveForm->addRow(QString::fromUtf8("Ось:"), axisRow);
  revolveForm->addRow(QString::fromUtf8("Угол:"), revolveAngleSpin_);
  revolveForm->addRow(QString::fromUtf8("Операция:"), revolveOperationCombo_);
  revolveForm->addRow(QString(), revolveReverseCheck_);
  auto* revolveButtons = new QHBoxLayout;
  auto* cancelRevolve = new QPushButton(QString::fromUtf8("Отмена"), revolvePanel);
  revolveAcceptButton_ = new QPushButton(QString::fromUtf8("Применить"), revolvePanel);
  revolveAcceptButton_->setEnabled(false);
  revolveAcceptButton_->setDefault(true);
  revolveAcceptButton_->setProperty("uiRole", "primaryAction");
  revolveAcceptButton_->setObjectName("primaryAction");
  revolveButtons->addWidget(cancelRevolve);
  revolveButtons->addWidget(revolveAcceptButton_);
  revolveLayout->addWidget(revolveTitle);
  revolveLayout->addWidget(revolveDescription);
  revolveLayout->addLayout(revolveForm);
  revolveLayout->addWidget(revolveStepHint_);
  revolveLayout->addStretch();
  revolveLayout->addLayout(revolveButtons);
  revolveDock_->setWidget(revolvePanel);
  revolveDock_->setObjectName(QStringLiteral("revolveParametersDock"));
  revolveDock_->setMinimumWidth(280);
  addDockWidget(Qt::RightDockWidgetArea, revolveDock_);
  revolveDock_->hide();
  connect(revolveProfileCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            if (!partProjectActive()) return;
            invalidatePreviewUpdates();
            rebuildRevolveAxisChoices();
            partDesignCoordinator_.clearRevolveAxis(activePartDocument());
            const auto id = static_cast<SketchId>(revolveProfileCombo_->currentData().toULongLong());
            if (id == kInvalidSketchId) {
              partDesignCoordinator_.clearRevolveProfile(activePartDocument());
              revolveProfileSummary_->setText(
                  QString::fromUtf8("Профиль не выбран"));
              viewport_->beginExtrusionSurfaceSelection();
            } else {
              partDesignCoordinator_.setRevolveProfile(activePartDocument(), id);
              revolveProfileSummary_->setText(
                  QString::fromUtf8("Выбран весь эскиз"));
              const auto found = std::find_if(
                  sketchViews_.begin(), sketchViews_.end(),
                  [id](const auto& entry) {
                    return entry.documentSketchId == id;
                  });
              if (found != sketchViews_.end())
                viewport_->beginRevolveAxisSelection(static_cast<std::size_t>(
                    std::distance(sketchViews_.begin(), found)));
            }
            updateRevolveToolPreview();
          });
  connect(revolveAxisCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            if (!partProjectActive()) return;
            invalidatePreviewUpdates();
            if (revolveAxisCombo_->currentIndex() <= 0) {
              partDesignCoordinator_.clearRevolveAxis(activePartDocument()); updateRevolveToolPreview(); return;
            }
            AxisReference reference;
            const qulonglong value = revolveAxisCombo_->currentData().toULongLong();
            if (value == Viewport::kGlobalXAxisToken)
              reference.type = AxisReferenceType::GlobalX;
            else if (value == Viewport::kGlobalYAxisToken)
              reference.type = AxisReferenceType::GlobalY;
            else if (value == Viewport::kGlobalZAxisToken)
              reference.type = AxisReferenceType::GlobalZ;
            else {
              reference.sketchId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileSketchId;
              if (value == 1)
                reference.type = AxisReferenceType::SketchHorizontalAxis;
              else if (value == 2)
                reference.type = AxisReferenceType::SketchVerticalAxis;
              else {
                reference.type = AxisReferenceType::SketchLine;
                reference.lineId = static_cast<sketch::GeometryId>(value - 3);
              }
            }
            partDesignCoordinator_.setRevolveAxis(activePartDocument(), reference); updateRevolveToolPreview();
          });
  connect(revolveAngleSpin_, &QDoubleSpinBox::valueChanged, this,
          [this](double value) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle == ToolLifecycle::Inactive)
              return;
            auto effect = std::make_shared<PartDesignUiEffect>();
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).sourceFeatureId,
                [this, value, effect] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle !=
                      ToolLifecycle::Inactive)
                    *effect = partDesignCoordinator_.dispatchActiveInput(
                        activePartDocument(), value, PartDesignParameterSource::Panel);
                },
                [this, effect] { applyPartDesignUiEffect(std::move(*effect)); });
          });
  connect(revolveOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int value) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle == ToolLifecycle::Inactive)
              return;
            const auto operation = static_cast<ExtrudeOperation>(value);
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).sourceFeatureId,
                [this, operation] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setRevolveOperation(activePartDocument(), operation);
                },
                [this] { updateRevolveToolPreview(); });
          });
  connect(revolveReverseCheck_, &QCheckBox::toggled, this,
          [this](bool value) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle == ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setRevolveReversed(activePartDocument(), value);
                },
                [this] { updateRevolveToolPreview(); });
          });
  connect(revolveAcceptButton_, &QPushButton::clicked, this,
          [this] {
            if (!partProjectActive()) return;
            applyActivePartDesignTool();
          });
  connect(cancelRevolve, &QPushButton::clicked, this,
          &MainWindow::cancelRevolveTool);
  const auto acceptRevolveOnEnter = [this] {
    if (!partProjectActive()) return;
    if (!revolveDock_->isVisible()) return;
    revolveAngleSpin_->interpretText();
    flushPreviewUpdate();
    if (revolveAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  };
  auto* revolveReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), revolveDock_);
  revolveReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  revolveReturnShortcut->setAutoRepeat(false);
  connect(revolveReturnShortcut, &QShortcut::activated, this,
          acceptRevolveOnEnter);
  auto* revolveKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), revolveDock_);
  revolveKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  revolveKeypadShortcut->setAutoRepeat(false);
  connect(revolveKeypadShortcut, &QShortcut::activated, this,
          acceptRevolveOnEnter);
  auto* revolveEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), revolveDock_);
  revolveEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  revolveEscapeShortcut->setAutoRepeat(false);
  connect(revolveEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelRevolveTool);
  connect(reselectProfile, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    invalidatePreviewUpdates();
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingInput);
    partDesignCoordinator_.clearRevolveAxis(activePartDocument());
    partDesignCoordinator_.clearRevolveProfile(activePartDocument());
    viewport_->clearToolManipulator();
    {
      const QSignalBlocker blocker(revolveProfileCombo_);
      revolveProfileCombo_->setCurrentIndex(0);
    }
    revolveProfileSummary_->setText(QString::fromUtf8(
        "Щёлкните область в 3D-виде. Ctrl добавляет или убирает области."));
    viewport_->beginExtrusionSurfaceSelection();
    viewport_->setFocus();
    statusBar()->showMessage(QString::fromUtf8("1/3 Выберите новый профиль"));
  });
  connect(reselectAxis, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    invalidatePreviewUpdates();
    std::size_t sketchIndex = static_cast<std::size_t>(-1);
    const auto found = std::find_if(
        sketchViews_.begin(), sketchViews_.end(), [this](const auto& entry) {
          return entry.documentSketchId == partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileSketchId;
        });
    if (found != sketchViews_.end())
      sketchIndex = static_cast<std::size_t>(
          std::distance(sketchViews_.begin(), found));
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingReference);
    partDesignCoordinator_.clearRevolveAxis(activePartDocument());
    viewport_->clearToolManipulator();
    updateRevolveToolPreview();
    viewport_->beginRevolveAxisSelection(sketchIndex);
    viewport_->setFocus();
    statusBar()->showMessage(
        sketchIndex < sketchViews_.size()
            ? QString::fromUtf8("2/3 Выберите глобальную ось или линию эскиза")
            : QString::fromUtf8("Выберите глобальную ось; линии эскиза станут доступны после выбора профиля"));
  });
  connect(viewport_, &Viewport::angularToolManipulatorValueChanged, this,
          [this](double angle) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Revolve) {
              const QSignalBlocker blocker(revolveAngleSpin_);
              revolveAngleSpin_->setValue(angle);
              auto effect = std::make_shared<PartDesignUiEffect>();
              schedulePreviewUpdate(
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).bodyId,
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).sourceFeatureId,
                  [this, angle, effect] {
                    if (partDesignCoordinator_.activeTool() ==
                        PartDesignToolKind::Revolve)
                      *effect = partDesignCoordinator_.dispatchActiveInput(
                          activePartDocument(), angle,
                          PartDesignParameterSource::Manipulator);
                  },
                  [this, effect] {
                    applyPartDesignUiEffect(std::move(*effect));
                  });
            } else if (partDesignCoordinator_.activeTool() ==
                       PartDesignToolKind::CircularPattern) {
              const QSignalBlocker blocker(circularPatternAngleSpin_);
              circularPatternAngleSpin_->setValue(angle);
              schedulePreviewUpdate(
                  partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
                  partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
                  [this, angle] {
                    if (partDesignCoordinator_.activeTool() ==
                        PartDesignToolKind::CircularPattern)
                      partDesignCoordinator_.setCircularPatternAngle(angle);
                  },
                  [this] { updateCircularPatternToolPreview(); });
            }
          });
  connect(viewport_, &Viewport::revolveProfileSelectionChanged, this,
          &MainWindow::updateRevolveProfileSelection);

  mirrorDock_ = new QDockWidget(QString::fromUtf8("Зеркало"), this);
  mirrorDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  mirrorDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  mirrorDock_->setObjectName(QStringLiteral("mirrorParametersDock"));
  auto* mirrorPanel = new QWidget(mirrorDock_);
  auto* mirrorLayout = new QVBoxLayout(mirrorPanel);
  mirrorLayout->setContentsMargins(16, 14, 16, 14);
  mirrorLayout->setSpacing(12);
  const auto* mirrorHelp = partDesignToolHelp(PartDesignToolKind::Mirror);
  auto* mirrorTitle = new QLabel(mirrorHelp->title.toUpper(), mirrorPanel);
  QFont mirrorTitleFont = mirrorTitle->font();
  mirrorTitleFont.setBold(true);
  mirrorTitle->setFont(mirrorTitleFont);
  auto* mirrorDescription = new QLabel(mirrorHelp->shortDescription,
                                       mirrorPanel);
  mirrorDescription->setWordWrap(true);
  mirrorDescription->setProperty("uiRole", "secondaryText");
  auto* mirrorForm = new QFormLayout;

  auto* mirrorBodyRow = new QWidget(mirrorPanel);
  auto* mirrorBodyLayout = new QHBoxLayout(mirrorBodyRow);
  mirrorBodyLayout->setContentsMargins(0, 0, 0, 0);
  mirrorBodyValue_ = new QLabel(QString::fromUtf8("Не выбрано"), mirrorBodyRow);
  mirrorBodyValue_->setObjectName(QStringLiteral("mirrorBodyValue"));
  mirrorBodySelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), mirrorBodyRow);
  mirrorBodySelectButton_->setObjectName(
      QStringLiteral("mirrorBodySelectButton"));
  mirrorBodyLayout->addWidget(mirrorBodyValue_, 1);
  mirrorBodyLayout->addWidget(mirrorBodySelectButton_);

  auto* mirrorPlaneRow = new QWidget(mirrorPanel);
  auto* mirrorPlaneLayout = new QHBoxLayout(mirrorPlaneRow);
  mirrorPlaneLayout->setContentsMargins(0, 0, 0, 0);
  mirrorPlaneValue_ = new QLabel(QString::fromUtf8("Не выбрана"),
                                 mirrorPlaneRow);
  mirrorPlaneValue_->setObjectName(QStringLiteral("mirrorPlaneValue"));
  mirrorPlaneSelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), mirrorPlaneRow);
  mirrorPlaneSelectButton_->setObjectName(
      QStringLiteral("mirrorPlaneSelectButton"));
  mirrorPlaneSelectButton_->setEnabled(false);
  mirrorPlaneLayout->addWidget(mirrorPlaneValue_, 1);
  mirrorPlaneLayout->addWidget(mirrorPlaneSelectButton_);
  mirrorForm->addRow(QString::fromUtf8("Тело:"), mirrorBodyRow);
  mirrorForm->addRow(QString::fromUtf8("Плоскость:"), mirrorPlaneRow);

  mirrorStepHint_ = new QLabel(mirrorHelp->selectionHint, mirrorPanel);
  mirrorStepHint_->setObjectName(QStringLiteral("mirrorStepHint"));
  mirrorStepHint_->setWordWrap(true);
  mirrorStepHint_->setProperty("uiRole", "secondaryText");
  auto* mirrorButtons = new QHBoxLayout;
  auto* cancelMirror = new QPushButton(QString::fromUtf8("Отмена"), mirrorPanel);
  mirrorAcceptButton_ = new QPushButton(QString::fromUtf8("Применить"),
                                        mirrorPanel);
  mirrorAcceptButton_->setObjectName(QStringLiteral("primaryAction"));
  mirrorAcceptButton_->setProperty("uiRole", "primaryAction");
  mirrorAcceptButton_->setDefault(true);
  mirrorAcceptButton_->setEnabled(false);
  mirrorButtons->addWidget(cancelMirror);
  mirrorButtons->addWidget(mirrorAcceptButton_);
  mirrorLayout->addWidget(mirrorTitle);
  mirrorLayout->addWidget(mirrorDescription);
  mirrorLayout->addLayout(mirrorForm);
  mirrorLayout->addWidget(mirrorStepHint_);
  mirrorLayout->addStretch();
  mirrorLayout->addLayout(mirrorButtons);
  mirrorDock_->setWidget(mirrorPanel);
  mirrorDock_->setMinimumWidth(280);
  addDockWidget(Qt::RightDockWidgetArea, mirrorDock_);
  mirrorDock_->hide();

  connect(mirrorBodySelectButton_, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    invalidatePreviewUpdates();
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingInput);
    partDesignCoordinator_.clearMirrorBody();
    mirrorBodyValue_->setText(QString::fromUtf8("Не выбрано"));
    mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
    mirrorPlaneSelectButton_->setEnabled(false);
    viewport_->clearToolPreviewShape();
    viewport_->beginMirrorBodySelection();
    updateMirrorToolPreview();
    viewport_->setFocus();
    statusBar()->showMessage(QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
  });
  connect(mirrorPlaneSelectButton_, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    invalidatePreviewUpdates();
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).bodyId == kInvalidBodyId) return;
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingReference);
    partDesignCoordinator_.clearMirrorPlane();
    mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
    viewport_->clearToolPreviewShape();
    viewport_->beginMirrorPlaneSelection();
    updateMirrorToolPreview();
    viewport_->setFocus();
    statusBar()->showMessage(
        QString::fromUtf8(
            "2/2 Выберите базовую плоскость или плоскую грань в 3D-виде"));
  });
  connect(mirrorAcceptButton_, &QPushButton::clicked, this,
          [this] {
            if (!partProjectActive()) return;
            applyActivePartDesignTool();
          });
  connect(cancelMirror, &QPushButton::clicked, this,
          &MainWindow::cancelMirrorTool);
  auto* mirrorReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), mirrorDock_);
  mirrorReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  mirrorReturnShortcut->setAutoRepeat(false);
  connect(mirrorReturnShortcut, &QShortcut::activated, this, [this] {
    if (!partProjectActive()) return;
    if (mirrorDock_->isVisible() && mirrorAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* mirrorKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), mirrorDock_);
  mirrorKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  mirrorKeypadShortcut->setAutoRepeat(false);
  connect(mirrorKeypadShortcut, &QShortcut::activated, this, [this] {
    if (!partProjectActive()) return;
    if (mirrorDock_->isVisible() && mirrorAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* mirrorEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), mirrorDock_);
  mirrorEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  mirrorEscapeShortcut->setAutoRepeat(false);
  connect(mirrorEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelMirrorTool);

  moveDock_ = new QDockWidget(QString::fromUtf8("Перемещение"), this);
  moveDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  moveDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  moveDock_->setObjectName(QStringLiteral("moveParametersDock"));
  auto* movePanel = new QWidget(moveDock_);
  auto* moveLayout = new QVBoxLayout(movePanel);
  moveLayout->setContentsMargins(16, 14, 16, 14);
  moveLayout->setSpacing(12);
  const auto* moveHelp = partDesignToolHelp(PartDesignToolKind::Move);
  auto* moveTitle = new QLabel(moveHelp->title.toUpper(), movePanel);
  QFont moveTitleFont = moveTitle->font();
  moveTitleFont.setBold(true);
  moveTitle->setFont(moveTitleFont);
  auto* moveDescription = new QLabel(moveHelp->shortDescription, movePanel);
  moveDescription->setWordWrap(true);
  moveDescription->setProperty("uiRole", "secondaryText");
  auto* moveForm = new QFormLayout;

  auto* moveBodyRow = new QWidget(movePanel);
  auto* moveBodyLayout = new QHBoxLayout(moveBodyRow);
  moveBodyLayout->setContentsMargins(0, 0, 0, 0);
  moveBodyValue_ = new QLabel(QString::fromUtf8("Не выбрано"), moveBodyRow);
  moveBodyValue_->setObjectName(QStringLiteral("moveBodyValue"));
  moveBodySelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), moveBodyRow);
  moveBodySelectButton_->setObjectName(QStringLiteral("moveBodySelectButton"));
  moveBodyLayout->addWidget(moveBodyValue_, 1);
  moveBodyLayout->addWidget(moveBodySelectButton_);

  const auto makeMoveSpin = [movePanel](const char* objectName) {
    auto* spin = new QDoubleSpinBox(movePanel);
    spin->setObjectName(QLatin1String(objectName));
    spin->setRange(-100000.0, 100000.0);
    spin->setDecimals(2);
    spin->setSingleStep(0.1);
    spin->setSuffix(QStringLiteral(" mm"));
    return spin;
  };
  moveXSpin_ = makeMoveSpin("moveXSpin");
  moveYSpin_ = makeMoveSpin("moveYSpin");
  moveZSpin_ = makeMoveSpin("moveZSpin");
  moveForm->addRow(QString::fromUtf8("Тело:"), moveBodyRow);
  moveForm->addRow(QStringLiteral("X:"), moveXSpin_);
  moveForm->addRow(QStringLiteral("Y:"), moveYSpin_);
  moveForm->addRow(QStringLiteral("Z:"), moveZSpin_);

  moveStepHint_ = new QLabel(moveHelp->selectionHint, movePanel);
  moveStepHint_->setObjectName(QStringLiteral("moveStepHint"));
  moveStepHint_->setWordWrap(true);
  moveStepHint_->setProperty("uiRole", "secondaryText");
  auto* moveButtons = new QHBoxLayout;
  auto* cancelMove = new QPushButton(QString::fromUtf8("Отмена"), movePanel);
  cancelMove->setObjectName(QStringLiteral("moveCancelButton"));
  moveAcceptButton_ = new QPushButton(QString::fromUtf8("Применить"), movePanel);
  moveAcceptButton_->setObjectName(QStringLiteral("moveAcceptButton"));
  moveAcceptButton_->setProperty("uiRole", "primaryAction");
  moveAcceptButton_->setDefault(true);
  moveAcceptButton_->setEnabled(false);
  moveButtons->addWidget(cancelMove);
  moveButtons->addWidget(moveAcceptButton_);
  moveLayout->addWidget(moveTitle);
  moveLayout->addWidget(moveDescription);
  moveLayout->addLayout(moveForm);
  moveLayout->addWidget(moveStepHint_);
  moveLayout->addStretch();
  moveLayout->addLayout(moveButtons);
  moveDock_->setWidget(movePanel);
  moveDock_->setMinimumWidth(280);
  addDockWidget(Qt::RightDockWidgetArea, moveDock_);
  moveDock_->hide();

  connect(moveBodySelectButton_, &QPushButton::clicked, this, [this] {
    if (!moveCommandContextActive()) return;
    invalidatePreviewUpdates();
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingInput);
    partDesignCoordinator_.clearMoveBody();
    moveBodyValue_->setText(QString::fromUtf8("Не выбрано"));
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    viewport_->beginMoveBodySelection();
    updateMoveToolPreview();
    viewport_->setFocus();
    statusBar()->showMessage(QString::fromUtf8("Выберите тело в 3D-виде"));
  });
  const auto moveValueChanged = [this](double) {
    if (!moveCommandContextActive()) return;
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive) return;
    const Vector3d offset{moveXSpin_->value(), moveYSpin_->value(),
                          moveZSpin_->value()};
    schedulePreviewUpdate(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId, partDesignCoordinator_.snapshot(PartDesignToolKind::Move).sourceFeatureId,
        [this, offset] {
          if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle != ToolLifecycle::Inactive)
            partDesignCoordinator_.setMoveOffset(offset);
        },
        [this] { updateMoveToolPreview(); });
  };
  connect(moveXSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveYSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveZSpin_, &QDoubleSpinBox::valueChanged, this, moveValueChanged);
  connect(moveAcceptButton_, &QPushButton::clicked, this,
          [this] {
            if (!moveCommandContextActive()) return;
            applyActivePartDesignTool();
          });
  connect(cancelMove, &QPushButton::clicked, this,
          &MainWindow::cancelMoveTool);
  const auto acceptMoveOnEnter = [this] {
    if (!moveCommandContextActive()) return;
    if (!moveDock_->isVisible()) return;
    moveXSpin_->interpretText();
    moveYSpin_->interpretText();
    moveZSpin_->interpretText();
    flushPreviewUpdate();
    if (moveAcceptButton_->isEnabled()) applyActivePartDesignTool();
  };
  auto* moveReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), moveDock_);
  moveReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  moveReturnShortcut->setAutoRepeat(false);
  connect(moveReturnShortcut, &QShortcut::activated, this,
          acceptMoveOnEnter);
  auto* moveKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), moveDock_);
  moveKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  moveKeypadShortcut->setAutoRepeat(false);
  connect(moveKeypadShortcut, &QShortcut::activated, this,
          acceptMoveOnEnter);
  auto* moveEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), moveDock_);
  moveEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  moveEscapeShortcut->setAutoRepeat(false);
  connect(moveEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelMoveTool);

  linearPatternDock_ =
      new QDockWidget(QString::fromUtf8("Линейный массив"), this);
  linearPatternDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  linearPatternDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  linearPatternDock_->setObjectName(
      QStringLiteral("linearPatternParametersDock"));
  auto* linearPatternPanel = new QWidget(linearPatternDock_);
  auto* linearPatternLayout = new QVBoxLayout(linearPatternPanel);
  linearPatternLayout->setContentsMargins(16, 14, 16, 14);
  linearPatternLayout->setSpacing(12);
  const auto* linearPatternHelp =
      partDesignToolHelp(PartDesignToolKind::LinearPattern);
  auto* linearPatternTitle =
      new QLabel(linearPatternHelp->title.toUpper(), linearPatternPanel);
  QFont linearPatternTitleFont = linearPatternTitle->font();
  linearPatternTitleFont.setBold(true);
  linearPatternTitle->setFont(linearPatternTitleFont);
  auto* linearPatternDescription =
      new QLabel(linearPatternHelp->shortDescription, linearPatternPanel);
  linearPatternDescription->setWordWrap(true);
  linearPatternDescription->setProperty("uiRole", "secondaryText");
  auto* linearPatternForm = new QFormLayout;

  auto* linearPatternBodyRow = new QWidget(linearPatternPanel);
  auto* linearPatternBodyLayout = new QHBoxLayout(linearPatternBodyRow);
  linearPatternBodyLayout->setContentsMargins(0, 0, 0, 0);
  linearPatternBodyValue_ =
      new QLabel(QString::fromUtf8("Не выбрано"), linearPatternBodyRow);
  linearPatternBodyValue_->setObjectName(
      QStringLiteral("linearPatternBodyValue"));
  linearPatternBodySelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), linearPatternBodyRow);
  linearPatternBodySelectButton_->setObjectName(
      QStringLiteral("linearPatternBodySelectButton"));
  linearPatternBodyLayout->addWidget(linearPatternBodyValue_, 1);
  linearPatternBodyLayout->addWidget(linearPatternBodySelectButton_);

  auto* linearPatternAxisRow = new QWidget(linearPatternPanel);
  auto* linearPatternAxisLayout = new QHBoxLayout(linearPatternAxisRow);
  linearPatternAxisLayout->setContentsMargins(0, 0, 0, 0);
  linearPatternAxisValue_ =
      new QLabel(QString::fromUtf8("Не выбрано"), linearPatternAxisRow);
  linearPatternAxisValue_->setObjectName(
      QStringLiteral("linearPatternAxisValue"));
  linearPatternAxisSelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), linearPatternAxisRow);
  linearPatternAxisSelectButton_->setObjectName(
      QStringLiteral("linearPatternAxisSelectButton"));
  linearPatternAxisSelectButton_->setEnabled(false);
  linearPatternAxisLayout->addWidget(linearPatternAxisValue_, 1);
  linearPatternAxisLayout->addWidget(linearPatternAxisSelectButton_);

  linearPatternSpacingSpin_ = new QDoubleSpinBox(linearPatternPanel);
  linearPatternSpacingSpin_->setObjectName(
      QStringLiteral("linearPatternSpacingSpin"));
  linearPatternSpacingSpin_->setRange(kMinimumPatternParameter,
                                      kMaximumPatternSpacingMm);
  linearPatternSpacingSpin_->setDecimals(2);
  linearPatternSpacingSpin_->setSingleStep(0.1);
  linearPatternSpacingSpin_->setSuffix(QStringLiteral(" mm"));
  linearPatternSpacingSpin_->setValue(30.0);
  linearPatternCountSpin_ = new QSpinBox(linearPatternPanel);
  linearPatternCountSpin_->setObjectName(
      QStringLiteral("linearPatternCountSpin"));
  linearPatternCountSpin_->setRange(kMinimumPatternCount,
                                    kMaximumPatternCount);
  linearPatternCountSpin_->setValue(3);
  linearPatternOperationCombo_ = new QComboBox(linearPatternPanel);
  linearPatternOperationCombo_->setObjectName(
      QStringLiteral("linearPatternOperationCombo"));
  linearPatternOperationCombo_->addItems(
      {QString::fromUtf8("Новое тело"), QString::fromUtf8("Добавить")});
  linearPatternForm->addRow(QString::fromUtf8("Тело:"), linearPatternBodyRow);
  linearPatternForm->addRow(QString::fromUtf8("Направление:"),
                            linearPatternAxisRow);
  linearPatternForm->addRow(QString::fromUtf8("Шаг:"),
                            linearPatternSpacingSpin_);
  linearPatternForm->addRow(QString::fromUtf8("Количество:"),
                            linearPatternCountSpin_);
  linearPatternForm->addRow(QString::fromUtf8("Операция:"),
                            linearPatternOperationCombo_);

  linearPatternStepHint_ =
      new QLabel(linearPatternHelp->selectionHint, linearPatternPanel);
  linearPatternStepHint_->setObjectName(
      QStringLiteral("linearPatternStepHint"));
  linearPatternStepHint_->setWordWrap(true);
  linearPatternStepHint_->setProperty("uiRole", "secondaryText");
  auto* linearPatternButtons = new QHBoxLayout;
  auto* cancelLinearPattern =
      new QPushButton(QString::fromUtf8("Отмена"), linearPatternPanel);
  linearPatternAcceptButton_ =
      new QPushButton(QString::fromUtf8("Применить"), linearPatternPanel);
  linearPatternAcceptButton_->setObjectName(QStringLiteral("primaryAction"));
  linearPatternAcceptButton_->setProperty("uiRole", "primaryAction");
  linearPatternAcceptButton_->setDefault(true);
  linearPatternAcceptButton_->setEnabled(false);
  linearPatternButtons->addWidget(cancelLinearPattern);
  linearPatternButtons->addWidget(linearPatternAcceptButton_);
  linearPatternLayout->addWidget(linearPatternTitle);
  linearPatternLayout->addWidget(linearPatternDescription);
  linearPatternLayout->addLayout(linearPatternForm);
  linearPatternLayout->addWidget(linearPatternStepHint_);
  linearPatternLayout->addStretch();
  linearPatternLayout->addLayout(linearPatternButtons);
  linearPatternDock_->setWidget(linearPatternPanel);
  linearPatternDock_->setMinimumWidth(300);
  addDockWidget(Qt::RightDockWidgetArea, linearPatternDock_);
  linearPatternDock_->hide();

  connect(linearPatternBodySelectButton_, &QPushButton::clicked, this,
          [this] {
            if (!partProjectActive()) return;
            invalidatePreviewUpdates();
            partDesignCoordinator_.beginReselection(
                ToolSelectionStage::SelectingInput);
            partDesignCoordinator_.clearLinearPatternBody();
            linearPatternBodyValue_->setText(QString::fromUtf8("Не выбрано"));
            linearPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
            linearPatternAxisSelectButton_->setEnabled(false);
            viewport_->clearToolPreviewShape();
            viewport_->clearToolManipulator();
            viewport_->beginLinearPatternBodySelection();
            updateLinearPatternToolPreview();
            viewport_->setFocus();
            statusBar()->showMessage(
                QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
          });
  connect(linearPatternAxisSelectButton_, &QPushButton::clicked, this,
          [this] {
            if (!partProjectActive()) return;
            invalidatePreviewUpdates();
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId == kInvalidBodyId) return;
            partDesignCoordinator_.beginReselection(
                ToolSelectionStage::SelectingReference);
            partDesignCoordinator_.clearLinearPatternDirection();
            linearPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
            viewport_->clearToolPreviewShape();
            viewport_->clearToolManipulator();
            viewport_->beginLinearPatternAxisSelection();
            updateLinearPatternToolPreview();
            viewport_->setFocus();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую ось в 3D-виде"));
          });
  connect(linearPatternSpacingSpin_,
          qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double value) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setLinearPatternSpacing(value);
                },
                [this] { updateLinearPatternToolPreview(); });
          });
  connect(linearPatternCountSpin_, qOverload<int>(&QSpinBox::valueChanged),
          this, [this](int value) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setLinearPatternCount(value);
                },
                [this] { updateLinearPatternToolPreview(); });
          });
  connect(linearPatternOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int index) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            const auto operation = index == 0 ? PatternOperation::NewBody
                                              : PatternOperation::Join;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
                [this, operation] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setLinearPatternOperation(operation);
                },
                [this] { updateLinearPatternToolPreview(); });
          });
  connect(linearPatternAcceptButton_, &QPushButton::clicked, this,
          [this] {
            if (!partProjectActive()) return;
            applyActivePartDesignTool();
          });
  connect(cancelLinearPattern, &QPushButton::clicked, this,
          &MainWindow::cancelLinearPatternTool);
  auto* linearPatternReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), linearPatternDock_);
  linearPatternReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  linearPatternReturnShortcut->setAutoRepeat(false);
  connect(linearPatternReturnShortcut, &QShortcut::activated, this, [this] {
    if (!partProjectActive()) return;
    flushPreviewUpdate();
    if (linearPatternDock_->isVisible() &&
        linearPatternAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* linearPatternKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), linearPatternDock_);
  linearPatternKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  linearPatternKeypadShortcut->setAutoRepeat(false);
  connect(linearPatternKeypadShortcut, &QShortcut::activated, this, [this] {
    if (!partProjectActive()) return;
    flushPreviewUpdate();
    if (linearPatternDock_->isVisible() &&
        linearPatternAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* linearPatternEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), linearPatternDock_);
  linearPatternEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  linearPatternEscapeShortcut->setAutoRepeat(false);
  connect(linearPatternEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelLinearPatternTool);

  circularPatternDock_ =
      new QDockWidget(QString::fromUtf8("Круговой массив"), this);
  circularPatternDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  circularPatternDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  circularPatternDock_->setObjectName(
      QStringLiteral("circularPatternParametersDock"));
  auto* circularPatternPanel = new QWidget(circularPatternDock_);
  auto* circularPatternLayout = new QVBoxLayout(circularPatternPanel);
  circularPatternLayout->setContentsMargins(16, 14, 16, 14);
  circularPatternLayout->setSpacing(12);
  const auto* circularPatternHelp =
      partDesignToolHelp(PartDesignToolKind::CircularPattern);
  auto* circularPatternTitle =
      new QLabel(circularPatternHelp->title.toUpper(), circularPatternPanel);
  QFont circularPatternTitleFont = circularPatternTitle->font();
  circularPatternTitleFont.setBold(true);
  circularPatternTitle->setFont(circularPatternTitleFont);
  auto* circularPatternDescription =
      new QLabel(circularPatternHelp->shortDescription, circularPatternPanel);
  circularPatternDescription->setWordWrap(true);
  circularPatternDescription->setProperty("uiRole", "secondaryText");
  auto* circularPatternForm = new QFormLayout;

  auto* circularPatternBodyRow = new QWidget(circularPatternPanel);
  auto* circularPatternBodyLayout = new QHBoxLayout(circularPatternBodyRow);
  circularPatternBodyLayout->setContentsMargins(0, 0, 0, 0);
  circularPatternBodyValue_ =
      new QLabel(QString::fromUtf8("Не выбрано"), circularPatternBodyRow);
  circularPatternBodyValue_->setObjectName(
      QStringLiteral("circularPatternBodyValue"));
  circularPatternBodySelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), circularPatternBodyRow);
  circularPatternBodySelectButton_->setObjectName(
      QStringLiteral("circularPatternBodySelectButton"));
  circularPatternBodyLayout->addWidget(circularPatternBodyValue_, 1);
  circularPatternBodyLayout->addWidget(circularPatternBodySelectButton_);

  auto* circularPatternAxisRow = new QWidget(circularPatternPanel);
  auto* circularPatternAxisLayout = new QHBoxLayout(circularPatternAxisRow);
  circularPatternAxisLayout->setContentsMargins(0, 0, 0, 0);
  circularPatternAxisValue_ =
      new QLabel(QString::fromUtf8("Не выбрано"), circularPatternAxisRow);
  circularPatternAxisValue_->setObjectName(
      QStringLiteral("circularPatternAxisValue"));
  circularPatternAxisSelectButton_ = new QPushButton(
      QString::fromUtf8("Выбрать в 3D"), circularPatternAxisRow);
  circularPatternAxisSelectButton_->setObjectName(
      QStringLiteral("circularPatternAxisSelectButton"));
  circularPatternAxisSelectButton_->setEnabled(false);
  circularPatternAxisLayout->addWidget(circularPatternAxisValue_, 1);
  circularPatternAxisLayout->addWidget(circularPatternAxisSelectButton_);

  circularPatternAngleSpin_ = new QDoubleSpinBox(circularPatternPanel);
  circularPatternAngleSpin_->setObjectName(
      QStringLiteral("circularPatternAngleSpin"));
  circularPatternAngleSpin_->setRange(kMinimumPatternParameter,
                                      kMaximumPatternAngleDeg);
  circularPatternAngleSpin_->setDecimals(2);
  circularPatternAngleSpin_->setSingleStep(1.0);
  circularPatternAngleSpin_->setSuffix(QString::fromUtf8(" °"));
  circularPatternAngleSpin_->setValue(kMaximumPatternAngleDeg);
  circularPatternCountSpin_ = new QSpinBox(circularPatternPanel);
  circularPatternCountSpin_->setObjectName(
      QStringLiteral("circularPatternCountSpin"));
  circularPatternCountSpin_->setRange(kMinimumPatternCount,
                                      kMaximumPatternCount);
  circularPatternCountSpin_->setValue(4);
  circularPatternOperationCombo_ = new QComboBox(circularPatternPanel);
  circularPatternOperationCombo_->setObjectName(
      QStringLiteral("circularPatternOperationCombo"));
  circularPatternOperationCombo_->addItems(
      {QString::fromUtf8("Новое тело"), QString::fromUtf8("Добавить")});
  circularPatternForm->addRow(QString::fromUtf8("Тело:"),
                              circularPatternBodyRow);
  circularPatternForm->addRow(QString::fromUtf8("Ось:"),
                              circularPatternAxisRow);
  circularPatternForm->addRow(QString::fromUtf8("Угол:"),
                              circularPatternAngleSpin_);
  circularPatternForm->addRow(QString::fromUtf8("Количество:"),
                              circularPatternCountSpin_);
  circularPatternForm->addRow(QString::fromUtf8("Операция:"),
                              circularPatternOperationCombo_);

  circularPatternStepHint_ =
      new QLabel(circularPatternHelp->selectionHint, circularPatternPanel);
  circularPatternStepHint_->setObjectName(
      QStringLiteral("circularPatternStepHint"));
  circularPatternStepHint_->setWordWrap(true);
  circularPatternStepHint_->setProperty("uiRole", "secondaryText");
  auto* circularPatternButtons = new QHBoxLayout;
  auto* cancelCircularPattern =
      new QPushButton(QString::fromUtf8("Отмена"), circularPatternPanel);
  circularPatternAcceptButton_ =
      new QPushButton(QString::fromUtf8("Применить"), circularPatternPanel);
  circularPatternAcceptButton_->setObjectName(QStringLiteral("primaryAction"));
  circularPatternAcceptButton_->setProperty("uiRole", "primaryAction");
  circularPatternAcceptButton_->setDefault(true);
  circularPatternAcceptButton_->setEnabled(false);
  circularPatternButtons->addWidget(cancelCircularPattern);
  circularPatternButtons->addWidget(circularPatternAcceptButton_);
  circularPatternLayout->addWidget(circularPatternTitle);
  circularPatternLayout->addWidget(circularPatternDescription);
  circularPatternLayout->addLayout(circularPatternForm);
  circularPatternLayout->addWidget(circularPatternStepHint_);
  circularPatternLayout->addStretch();
  circularPatternLayout->addLayout(circularPatternButtons);
  circularPatternDock_->setWidget(circularPatternPanel);
  circularPatternDock_->setMinimumWidth(300);
  addDockWidget(Qt::RightDockWidgetArea, circularPatternDock_);
  circularPatternDock_->hide();

  connect(circularPatternBodySelectButton_, &QPushButton::clicked, this,
          [this] {
            if (!partProjectActive()) return;
            invalidatePreviewUpdates();
            partDesignCoordinator_.beginReselection(
                ToolSelectionStage::SelectingInput);
            partDesignCoordinator_.clearCircularPatternBody();
            circularPatternBodyValue_->setText(QString::fromUtf8("Не выбрано"));
            circularPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
            circularPatternAxisSelectButton_->setEnabled(false);
            viewport_->clearToolPreviewShape();
            viewport_->clearToolManipulator();
            viewport_->beginCircularPatternBodySelection();
            updateCircularPatternToolPreview();
            viewport_->setFocus();
            statusBar()->showMessage(
                QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
          });
  connect(circularPatternAxisSelectButton_, &QPushButton::clicked, this,
          [this] {
            if (!partProjectActive()) return;
            invalidatePreviewUpdates();
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId == kInvalidBodyId) return;
            partDesignCoordinator_.beginReselection(
                ToolSelectionStage::SelectingReference);
            partDesignCoordinator_.clearCircularPatternAxis();
            circularPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
            viewport_->clearToolPreviewShape();
            viewport_->clearToolManipulator();
            viewport_->beginCircularPatternAxisSelection();
            updateCircularPatternToolPreview();
            viewport_->setFocus();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую ось в 3D-виде"));
          });
  connect(circularPatternAngleSpin_,
          qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this](double value) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setCircularPatternAngle(value);
                },
                [this] { updateCircularPatternToolPreview(); });
          });
  connect(circularPatternCountSpin_, qOverload<int>(&QSpinBox::valueChanged),
          this, [this](int value) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
                [this, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setCircularPatternCount(value);
                },
                [this] { updateCircularPatternToolPreview(); });
          });
  connect(circularPatternOperationCombo_, &QComboBox::currentIndexChanged,
          this, [this](int index) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            const auto operation = index == 0 ? PatternOperation::NewBody
                                              : PatternOperation::Join;
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
                [this, operation] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
                      ToolLifecycle::Inactive)
                    partDesignCoordinator_.setCircularPatternOperation(operation);
                },
                [this] { updateCircularPatternToolPreview(); });
          });
  connect(circularPatternAcceptButton_, &QPushButton::clicked, this,
          [this] {
            if (!partProjectActive()) return;
            applyActivePartDesignTool();
          });
  connect(cancelCircularPattern, &QPushButton::clicked, this,
          &MainWindow::cancelCircularPatternTool);
  auto* circularPatternReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), circularPatternDock_);
  circularPatternReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  circularPatternReturnShortcut->setAutoRepeat(false);
  connect(circularPatternReturnShortcut, &QShortcut::activated, this, [this] {
    if (!partProjectActive()) return;
    flushPreviewUpdate();
    if (circularPatternDock_->isVisible() &&
        circularPatternAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* circularPatternKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), circularPatternDock_);
  circularPatternKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  circularPatternKeypadShortcut->setAutoRepeat(false);
  connect(circularPatternKeypadShortcut, &QShortcut::activated, this, [this] {
    if (!partProjectActive()) return;
    flushPreviewUpdate();
    if (circularPatternDock_->isVisible() &&
        circularPatternAcceptButton_->isEnabled())
      applyActivePartDesignTool();
  });
  auto* circularPatternEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), circularPatternDock_);
  circularPatternEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  circularPatternEscapeShortcut->setAutoRepeat(false);
  connect(circularPatternEscapeShortcut, &QShortcut::activated, this,
          &MainWindow::cancelCircularPatternTool);

  toolParametersDock_ = new QDockWidget(QString::fromUtf8("Параметры инструмента"), this);
  toolParametersDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  toolParametersDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  toolParametersPanel_ = new ToolParametersPanel(toolParametersDock_);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Fillet),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Радиус"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.0, 100000.0, 2);
  toolParametersDock_->setWidget(toolParametersPanel_);
  toolParametersDock_->setObjectName(QStringLiteral("partDesignParametersDock"));
  toolParametersDock_->setMinimumWidth(250);
  addDockWidget(Qt::RightDockWidgetArea, toolParametersDock_);
  toolParametersDock_->hide();
  connect(toolParametersPanel_, &ToolParametersPanel::parameterChanged, this,
          [this](double value) {
            if (!partProjectActive()) return;
            const auto kind = partDesignCoordinator_.activeTool();
            if (kind != PartDesignToolKind::Chamfer &&
                kind != PartDesignToolKind::Shell &&
                kind != PartDesignToolKind::Draft &&
                kind != PartDesignToolKind::Extrude &&
                kind != PartDesignToolKind::Fillet)
              return;
            const auto state = partDesignCoordinator_.snapshot(kind, activePartDocument());
            auto effect = std::make_shared<PartDesignUiEffect>();
            schedulePreviewUpdate(
                state.bodyId, state.sourceFeatureId,
                [this, kind, value, effect] {
                  if (partDesignCoordinator_.activeTool() == kind)
                    *effect = partDesignCoordinator_.dispatchActiveInput(
                        activePartDocument(), value, PartDesignParameterSource::Panel);
                },
                [this, effect] {
                  applyPartDesignUiEffect(std::move(*effect));
                });
          });
  connect(toolParametersPanel_, &ToolParametersPanel::accepted, this,
          [this] {
            if (!partProjectActive()) return;
            applyActivePartDesignTool();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::cancelled, this,
          [this] {
            if (!partProjectActive()) return;
            applyPartDesignUiEffect(partDesignCoordinator_.dispatchActiveAction(
                PartDesignAction::Cancel));
          });
  connect(toolParametersPanel_, &ToolParametersPanel::selectionRequested, this,
          [this] {
            if (!partProjectActive()) return;
            const auto kind = partDesignCoordinator_.activeTool();
            if (kind == PartDesignToolKind::None) return;
            invalidatePreviewUpdates();
            if (kind == PartDesignToolKind::JoinBodies)
              viewport_->beginJoinBodiesSelection();
            else if (kind == PartDesignToolKind::Draft) {
              partDesignCoordinator_.beginReselection(
                  ToolSelectionStage::SelectingInput);
              partDesignCoordinator_.setDraftFaces(activePartDocument(), {});
              partDesignCoordinator_.clearDraftPrincipalAxis(activePartDocument());
              viewport_->clearToolManipulator();
              viewport_->beginDraftFaceSelection();
              updateDraftToolPreview();
            }
            statusBar()->showMessage(QString::fromUtf8(
                "Выберите геометрию непосредственно в viewport"));
            viewport_->setFocus();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::clearSelectionRequested,
          this, [this] {
            if (!partProjectActive()) return;
            invalidatePreviewUpdates();
            applyPartDesignUiEffect(partDesignCoordinator_.dispatchActiveAction(
                PartDesignAction::ClearSelection, &activePartDocument()));
          });
  connect(viewport_, &Viewport::toolManipulatorValueChanged, this,
          [this](double value) {
            if (!partProjectActive()) return;
            const auto kind = partDesignCoordinator_.activeTool();
            const auto state = partDesignCoordinator_.snapshot(kind, activePartDocument());
            if (kind == PartDesignToolKind::LinearPattern) {
              const QSignalBlocker blocker(linearPatternSpacingSpin_);
              linearPatternSpacingSpin_->setValue(value);
              schedulePreviewUpdate(
                  state.bodyId, state.sourceFeatureId,
                  [this, kind, value] {
                    if (partDesignCoordinator_.activeTool() == kind)
                      partDesignCoordinator_.setLinearPatternSpacing(value);
                  },
                  [this] { updateLinearPatternToolPreview(); });
              return;
            }
            if (kind != PartDesignToolKind::Chamfer &&
                kind != PartDesignToolKind::Shell &&
                kind != PartDesignToolKind::Extrude &&
                kind != PartDesignToolKind::Fillet)
              return;
            toolParametersPanel_->setParameterValue(
                kind == PartDesignToolKind::Extrude ? std::abs(value) : value);
            auto effect = std::make_shared<PartDesignUiEffect>();
            schedulePreviewUpdate(
                state.bodyId, state.sourceFeatureId,
                [this, kind, value, effect] {
                  if (partDesignCoordinator_.activeTool() == kind)
                    *effect = partDesignCoordinator_.dispatchActiveInput(
                        activePartDocument(), value,
                        PartDesignParameterSource::Manipulator);
                },
                [this, effect] {
                  applyPartDesignUiEffect(std::move(*effect));
                });
          });
  connect(viewport_, &Viewport::bodyEdgeSelectionChanged, this, [this] {
    if (!partProjectActive()) return;
    invalidatePreviewUpdates();
    if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Chamfer) {
      partDesignCoordinator_.setChamferEdges(viewport_->selectedBodyEdges());
      updateChamferToolPreview();
      if (!partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).edges.empty())
        static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Fillet) {
      partDesignCoordinator_.setFilletEdges(viewport_->selectedBodyEdges());
      updateFilletToolPreview();
      if (!partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).edges.empty())
        static_cast<void>(viewport_->focusToolParameterField(false));
    }
  });
  connect(viewport_, &Viewport::bodyFaceSelectionChanged, this, [this] {
    if (!partProjectActive()) return;
    invalidatePreviewUpdates();
    if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Extrude) {
      const auto faces = viewport_->selectedBodyFaces();
      if (!faces.empty())
        partDesignCoordinator_.setExtrudeFace(faces.front());
      updateFaceExtrudeToolPreview();
      // Keep keyboard focus in the viewport after face selection so the next
      // Tab enters the on-canvas distance field, not the right-hand dock.
      static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Shell) {
      partDesignCoordinator_.setShellRemovedFaces(viewport_->selectedBodyFaces());
      updateShellToolPreview();
      // Keep keyboard focus in the viewport after face selection so the next
      // Tab enters the on-canvas thickness field, not the right-hand dock.
      static_cast<void>(viewport_->focusToolParameterField(false));
    } else if (partDesignCoordinator_.activeTool() == PartDesignToolKind::Draft) {
      auto faces = viewport_->selectedBodyFaces();
      if (faces.size() > 1) faces.resize(1);
      partDesignCoordinator_.setDraftFaces(activePartDocument(), faces);
      partDesignCoordinator_.clearDraftPrincipalAxis(activePartDocument());
      updateDraftToolPreview();
      if (!faces.empty()) {
        partDesignCoordinator_.beginReselection(
            ToolSelectionStage::SelectingReference);
        viewport_->beginDraftAxisSelection();
        statusBar()->showMessage(
            QString::fromUtf8(
                "2/3 Выберите ось X, Y, Z или прилегающее прямое ребро"));
      }
    }
  });
  connect(viewport_, &Viewport::bodiesSelected, this,
          [this](const std::vector<BodyId>& bodyIds) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle == ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            std::vector<JoinBodyInput> inputs;
            inputs.reserve(std::min<std::size_t>(bodyIds.size(), 2));
            for (const BodyId bodyId : bodyIds) {
              if (inputs.size() == 2) break;
              if (const auto editingId =
                      partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).editingFeatureId) {
                const Body* owner = nullptr;
                for (const auto& candidate : activePartDocument().bodies())
                  if (candidate.featureIndex(*editingId)) {
                    owner = &candidate;
                    break;
                  }
                if (owner && owner->id() == bodyId) continue;
              }
              Body* body = mutableStandaloneDocument().findBody(bodyId);
              if (!body || !body->activeFeature() || !body->resultShape())
                continue;
              inputs.push_back({bodyId, body->activeFeature()->id(),
                                body->resultShape()});
            }
            partDesignCoordinator_.setJoinBodies(std::move(inputs));
            updateJoinBodiesToolPreview();
          });
  connect(toolParametersPanel_, &ToolParametersPanel::optionChanged, this,
          [this](bool checked) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle != ToolLifecycle::Inactive) {
              const auto operation = checked ? ExtrudeOperation::Cut
                                             : ExtrudeOperation::Join;
              schedulePreviewUpdate(
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).bodyId,
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).sourceFeatureId,
                  [this, operation] {
                    if (partDesignCoordinator_.activeTool() ==
                        PartDesignToolKind::Extrude)
                      partDesignCoordinator_.setExtrudeOperation(operation);
                  },
                  [this] { updateFaceExtrudeToolPreview(); });
            } else if (partDesignCoordinator_.activeTool() ==
                       PartDesignToolKind::Shell) {
              schedulePreviewUpdate(
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).bodyId,
                  partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).sourceFeatureId,
                  [this, checked] {
                    if (partDesignCoordinator_.activeTool() ==
                        PartDesignToolKind::Shell)
                      partDesignCoordinator_.setShellOutside(checked);
                  },
                  [this] { updateShellToolPreview(); });
            }
          });
  connect(viewport_, &Viewport::angularToolManipulatorValueChanged, this,
          [this](double value) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.activeTool() != PartDesignToolKind::Draft)
              return;
            toolParametersPanel_->setParameterValue(value);
            auto effect = std::make_shared<PartDesignUiEffect>();
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).bodyId,
                partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).sourceFeatureId,
                [this, value, effect] {
                  if (partDesignCoordinator_.activeTool() ==
                      PartDesignToolKind::Draft)
                    *effect = partDesignCoordinator_.dispatchActiveInput(
                        activePartDocument(), value,
                        PartDesignParameterSource::Manipulator);
                },
                [this, effect] {
                  applyPartDesignUiEffect(std::move(*effect));
                });
          });
  connect(viewport_, &Viewport::toolManipulatorDragFinished, this, [this] {
    if (!partProjectActive()) return;
    if (viewport_->referenceImageManipulatorActive()) {
      commitReferenceImageEdit();
      return;
    }
    flushPreviewUpdate();
  });
  connect(viewport_, &Viewport::extrusionManipulatorDragFinished, this,
          [this] {
            if (!partProjectActive()) return;
            flushPreviewUpdate();
          });
  // HUD Enter commit: the value is already interpreted + preview-synced via the
  // value routing above, so perform the active tool's existing Accept exactly
  // like the Готово button. Each accept*Tool guards its own lifecycle, so an
  // invalid preview will not accept and focus stays in the HUD field.
  connect(viewport_, &Viewport::toolParameterCommitted, this, [this] {
    if (!partProjectActive()) return;
    if (viewport_->referenceImageManipulatorActive()) {
      commitReferenceImageEdit();
      return;
    }
    flushPreviewUpdate();
    // Extrude still uses its dedicated on-canvas spinbox. Treat Enter there
    // exactly like the Apply button before dispatching ToolSession tools.
    if (extrusionDock_->isVisible()) {
      extrudeSketch();
      return;
    }
    applyActivePartDesignTool();
  });

  const auto acceptActiveTool = [this] {
    if (!partProjectActive()) return;
    toolParametersPanel_->interpretParameterText();
    flushPreviewUpdate();
    if (!toolParametersPanel_->acceptEnabled()) return;
    applyActivePartDesignTool();
  };
  auto* acceptReturnShortcut =
      new QShortcut(QKeySequence(Qt::Key_Return), toolParametersDock_);
  acceptReturnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  acceptReturnShortcut->setAutoRepeat(false);
  connect(acceptReturnShortcut, &QShortcut::activated, this, acceptActiveTool);
  auto* acceptKeypadShortcut =
      new QShortcut(QKeySequence(Qt::Key_Enter), toolParametersDock_);
  acceptKeypadShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  acceptKeypadShortcut->setAutoRepeat(false);
  connect(acceptKeypadShortcut, &QShortcut::activated, this, acceptActiveTool);
  auto* cancelToolShortcut = new QShortcut(QKeySequence(Qt::Key_Escape),
                                           toolParametersDock_);
  cancelToolShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  cancelToolShortcut->setAutoRepeat(false);
  connect(cancelToolShortcut, &QShortcut::activated, this,
          [this] {
            if (!partProjectActive()) return;
            applyPartDesignUiEffect(partDesignCoordinator_.dispatchActiveAction(
                PartDesignAction::Escape));
          });
  // CAD Tab belongs to the on-canvas HUD, not to the controls in the
  // right-hand Part Design panel. WidgetWithChildrenShortcut catches Tab even
  // when a spinbox/checkbox inside the dock currently owns keyboard focus.
  const auto focusViewportHud = [this](bool backward) {
    if (!partProjectActive()) return;
    static_cast<void>(viewport_->focusToolParameterField(backward));
  };
  auto* hudTabShortcut =
      new QShortcut(QKeySequence(Qt::Key_Tab), toolParametersDock_);
  hudTabShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  hudTabShortcut->setAutoRepeat(false);
  connect(hudTabShortcut, &QShortcut::activated, this,
          [focusViewportHud] { focusViewportHud(false); });

  auto* hudBacktabShortcut =
      new QShortcut(QKeySequence(Qt::Key_Backtab), toolParametersDock_);
  hudBacktabShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  hudBacktabShortcut->setAutoRepeat(false);
  connect(hudBacktabShortcut, &QShortcut::activated, this,
          [focusViewportHud] { focusViewportHud(true); });
  connect(extrusionLengthSpin_, &QDoubleSpinBox::valueChanged, this,
          [this](double value) {
            if (!partProjectActive()) return;
            const double signedValue = extrusionReverseCheck_->isChecked()
                                           ? -std::abs(value)
                                           : std::abs(value);
            viewport_->setExtrusionPreviewLength(signedValue);
            updateLegacyExtrusionPreview();
          });
  connect(extrusionLengthSpin_, &QDoubleSpinBox::editingFinished, this,
          &MainWindow::normalizeExtrusionDistance);
  connect(extrusionReverseCheck_, &QCheckBox::toggled, this, [this](bool) {
    if (!partProjectActive()) return;
    viewport_->setExtrusionPreviewLength(
        extrusionReverseCheck_->isChecked()
            ? -std::abs(extrusionLengthSpin_->value())
            : std::abs(extrusionLengthSpin_->value()));
    updateLegacyExtrusionPreview();
  });
  connect(extrusionOperationCombo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            if (!partProjectActive()) return;
            extrudeOperationManuallyChanged_ = true;
            invalidatePreviewUpdates();
            updateLegacyExtrusionPreview();
          });
  connect(viewport_, &Viewport::extrusionPreviewLengthChanged, this,
          [this](double value) {
            if (!partProjectActive()) return;
            const QSignalBlocker distanceBlocker(extrusionLengthSpin_);
            const QSignalBlocker reverseBlocker(extrusionReverseCheck_);
            extrusionLengthSpin_->setValue(std::abs(value));
            extrusionReverseCheck_->setChecked(value < 0.0);
            scheduleAutomaticExtrudeOperation();
            updateLegacyExtrusionPreview();
          });
  connect(acceptExtrusion, &QPushButton::clicked, this, &MainWindow::extrudeSketch);
  const auto applyExtrusionOnEnter = [this] {
    if (!partProjectActive()) return;
    if (!extrusionDock_->isVisible()) return;
    extrusionLengthSpin_->interpretText();
    extrudeSketch();
  };
  auto* returnShortcut = new QShortcut(QKeySequence(Qt::Key_Return), extrusionDock_);
  returnShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  returnShortcut->setAutoRepeat(false);
  connect(returnShortcut, &QShortcut::activated, this, applyExtrusionOnEnter);
  auto* keypadEnterShortcut = new QShortcut(QKeySequence(Qt::Key_Enter), extrusionDock_);
  keypadEnterShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  keypadEnterShortcut->setAutoRepeat(false);
  connect(keypadEnterShortcut, &QShortcut::activated, this,
          applyExtrusionOnEnter);
  connect(cancelExtrusion, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    cancelLegacyExtrusion();
  });
  auto* extrusionEscapeShortcut =
      new QShortcut(QKeySequence(Qt::Key_Escape), extrusionDock_);
  extrusionEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  extrusionEscapeShortcut->setAutoRepeat(false);
  connect(extrusionEscapeShortcut, &QShortcut::activated, this, [this] {
    if (!partProjectActive()) return;
    cancelLegacyExtrusion();
  });

  sketchSettingsDock_ =
      new QDockWidget(QString::fromUtf8("Свойства эскиза"), this);
  sketchSettingsDock_->setObjectName(QStringLiteral("sketchSettingsDock"));
  sketchSettingsDock_->setAllowedAreas(Qt::RightDockWidgetArea);
  sketchSettingsDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* settingsPanel = new QWidget(sketchSettingsDock_);
  auto* settingsLayout = new QVBoxLayout(settingsPanel);
  settingsLayout->setContentsMargins(16, 14, 16, 14);
  settingsLayout->setSpacing(12);
  auto* gridCheck = new QCheckBox(QString::fromUtf8("Сетка"), settingsPanel);
  gridCheck->setChecked(true);
  auto* snapCheck =
      new QCheckBox(QString::fromUtf8("Привязка к сетке"), settingsPanel);
  // Exact grid snapping is intentionally opt-in. CAD inference to endpoints,
  // midpoints and carrier geometry remains active independently.
  snapCheck->setChecked(false);
  auto* lineTypeLabel =
      new QLabel(QString::fromUtf8("Тип линии"), settingsPanel);
  sketchLineTypeCombo_ = new QComboBox(settingsPanel);
  sketchLineTypeCombo_->addItem(QString::fromUtf8("Сплошная линия"));
  sketchLineTypeCombo_->addItem(QString::fromUtf8("Пунктирная линия"));
  sketchLineTypeCombo_->setEnabled(false);
  auto* orientSketchPlane = new QPushButton(
      QString::fromUtf8("Вернуть исходную ориентацию"), settingsPanel);
  orientSketchPlane->setObjectName(QStringLiteral("sketchOrientToPlaneButton"));
  orientSketchPlane->setToolTip(QString::fromUtf8(
      "Сбросить свободный поворот и вернуть камеру перпендикулярно "
      "исходной плоскости эскиза"));
  settingsLayout->addWidget(gridCheck);
  settingsLayout->addWidget(snapCheck);
  settingsLayout->addSpacing(8);
  settingsLayout->addWidget(lineTypeLabel);
  settingsLayout->addWidget(sketchLineTypeCombo_);
  settingsLayout->addWidget(orientSketchPlane);

  auto* constraintsSection = new QFrame(settingsPanel);
  constraintsSection->setObjectName(QStringLiteral("constraintsSection"));
  constraintsSection->setProperty("uiRole", "sectionPanel");
  auto* constraintsSectionLayout = new QVBoxLayout(constraintsSection);
  constraintsSectionLayout->setContentsMargins(0, 12, 0, 0);
  constraintsSectionLayout->setSpacing(7);
  auto* constraintsTitle =
      new QLabel(QString::fromUtf8("Ограничения"), constraintsSection);
  QFont constraintsTitleFont = constraintsTitle->font();
  constraintsTitleFont.setBold(true);
  constraintsTitle->setFont(constraintsTitleFont);
  sketchConstraintsList_ = new QTreeWidget(constraintsSection);
  sketchConstraintsList_->setColumnCount(2);
  sketchConstraintsList_->setHeaderHidden(true);
  sketchConstraintsList_->setRootIsDecorated(false);
  sketchConstraintsList_->setItemsExpandable(false);
  sketchConstraintsList_->setSelectionMode(QAbstractItemView::NoSelection);
  sketchConstraintsList_->setFocusPolicy(Qt::StrongFocus);
  sketchConstraintsList_->setMaximumHeight(150);
  sketchConstraintsList_->setColumnWidth(0, 185);
  sketchConstraintsList_->setColumnWidth(1, 28);

  constraintsSectionLayout->addWidget(constraintsTitle);
  constraintsSectionLayout->addWidget(sketchConstraintsList_);
  settingsLayout->addWidget(constraintsSection);

  auto* circlePropertiesSection = new QFrame(settingsPanel);
  circlePropertiesSection->setObjectName(QStringLiteral("circlePropertiesSection"));
  circlePropertiesSection->setProperty("uiRole", "sectionPanel");
  auto* circleLayout = new QFormLayout(circlePropertiesSection);
  circleLayout->setContentsMargins(0, 12, 0, 0);
  circleLayout->setSpacing(10);
  auto* circleTitle = new QLabel(QString::fromUtf8("Окружность"),
                                 circlePropertiesSection);
  QFont circleTitleFont = circleTitle->font();
  circleTitleFont.setBold(true);
  circleTitle->setFont(circleTitleFont);
  circleLayout->addRow(circleTitle);
  auto* circleDiameterSpin = new QDoubleSpinBox(circlePropertiesSection);
  circleDiameterSpin->setRange(0.01, 100000.0);
  circleDiameterSpin->setDecimals(2);
  circleDiameterSpin->setValue(20.0);
  circleDiameterSpin->setSuffix(QString::fromUtf8(" мм"));
  auto* circleModeButtons = new QWidget(circlePropertiesSection);
  auto* circleModeLayout = new QHBoxLayout(circleModeButtons);
  circleModeLayout->setContentsMargins(0, 0, 0, 0);
  circleModeLayout->setSpacing(4);
  auto* circleModeGroup = new QButtonGroup(circleModeButtons);
  circleModeGroup->setExclusive(true);
  struct CircleModeButtonSpec {
    ToolIconKind icon;
    const char* tooltip;
  };
  const CircleModeButtonSpec circleModes[] = {
      {ToolIconKind::CircleCenterRadius, "Из центра"},
      {ToolIconKind::CircleTwoPoints, "По двум точкам"},
      {ToolIconKind::CircleThreePoints, "По трём точкам"},
      {ToolIconKind::CircleThreeTangents, "По трём прямым"},
      {ToolIconKind::CircleTwoTangentsRadius,
       "По двум прямым и радиусу"},
  };
  for (int index = 0; index < 5; ++index) {
    auto* button = new QToolButton(circleModeButtons);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setIcon(toolIcon(circleModes[index].icon));
    button->setIconSize(QSize(26, 26));
    button->setFixedSize(36, 36);
    button->setToolTip(QString::fromUtf8(circleModes[index].tooltip));
    circleModeGroup->addButton(button, index);
    circleModeLayout->addWidget(button);
    if (index == 0) button->setChecked(true);
  }
  circleLayout->addRow(QString::fromUtf8("Диаметр:"), circleDiameterSpin);
  circleLayout->addRow(circleModeButtons);
  settingsLayout->addWidget(circlePropertiesSection);
  circlePropertiesSection->hide();

  auto* rectanglePropertiesSection = new QFrame(settingsPanel);
  rectanglePropertiesSection->setObjectName(
      QStringLiteral("rectanglePropertiesSection"));
  rectanglePropertiesSection->setProperty("uiRole", "sectionPanel");
  auto* rectangleLayout = new QVBoxLayout(rectanglePropertiesSection);
  rectangleLayout->setContentsMargins(0, 12, 0, 0);
  rectangleLayout->setSpacing(10);
  auto* rectangleTitle = new QLabel(QString::fromUtf8("Прямоугольник"),
                                    rectanglePropertiesSection);
  QFont rectangleTitleFont = rectangleTitle->font();
  rectangleTitleFont.setBold(true);
  rectangleTitle->setFont(rectangleTitleFont);
  rectangleLayout->addWidget(rectangleTitle);
  auto* rectangleModeButtons = new QWidget(rectanglePropertiesSection);
  auto* rectangleModeLayout = new QHBoxLayout(rectangleModeButtons);
  rectangleModeLayout->setContentsMargins(0, 0, 0, 0);
  rectangleModeLayout->setSpacing(4);
  auto* rectangleModeGroup = new QButtonGroup(rectangleModeButtons);
  rectangleModeGroup->setExclusive(true);
  const CircleModeButtonSpec rectangleModes[] = {
      {ToolIconKind::RectangleTwoPoints, "По двум точкам"},
      {ToolIconKind::RectangleThreePoints, "По трём точкам"},
      {ToolIconKind::RectangleFromCenter, "Из центра"},
  };
  for (int index = 0; index < 3; ++index) {
    auto* button = new QToolButton(rectangleModeButtons);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setIcon(toolIcon(rectangleModes[index].icon));
    button->setIconSize(QSize(26, 26));
    button->setFixedSize(36, 36);
    button->setToolTip(QString::fromUtf8(rectangleModes[index].tooltip));
    rectangleModeGroup->addButton(button, index);
    rectangleModeLayout->addWidget(button);
    if (index == 0) button->setChecked(true);
  }
  rectangleLayout->addWidget(rectangleModeButtons);
  settingsLayout->addWidget(rectanglePropertiesSection);
  rectanglePropertiesSection->hide();
  settingsLayout->addStretch();
  sketchSettingsDock_->setWidget(settingsPanel);
  sketchSettingsDock_->setMinimumWidth(250);
  addDockWidget(Qt::RightDockWidgetArea, sketchSettingsDock_);
  sketchSettingsDock_->hide();

  sketchMirrorDock_ =
      new QDockWidget(QString::fromUtf8("Зеркало эскиза"), this);
  sketchMirrorDock_->setObjectName(QStringLiteral("sketchMirrorParametersDock"));
  sketchMirrorDock_->setAllowedAreas(Qt::LeftDockWidgetArea);
  sketchMirrorDock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  auto* sketchMirrorPanel = new QWidget(sketchMirrorDock_);
  auto* sketchMirrorLayout = new QVBoxLayout(sketchMirrorPanel);
  sketchMirrorLayout->setContentsMargins(16, 14, 16, 14);
  sketchMirrorLayout->setSpacing(10);

  auto* mirrorHint = new QLabel(
      QString::fromUtf8(
          "Выберите элементы. Удерживайте Ctrl, чтобы добавить несколько. "
          "Затем выберите прямую-ось."),
      sketchMirrorPanel);
  mirrorHint->setWordWrap(true);
  sketchMirrorLayout->addWidget(mirrorHint);

  auto* elementsRow = new QHBoxLayout;
  auto* selectElements =
      new QPushButton(QString::fromUtf8("Выбрать элементы"), sketchMirrorPanel);
  selectElements->setObjectName(QStringLiteral("sketchMirrorSelectElementsButton"));
  auto* clearElements =
      new QPushButton(QString::fromUtf8("Сбросить"), sketchMirrorPanel);
  clearElements->setObjectName(QStringLiteral("sketchMirrorClearElementsButton"));
  elementsRow->addWidget(selectElements, 1);
  elementsRow->addWidget(clearElements);
  sketchMirrorLayout->addLayout(elementsRow);
  sketchMirrorElementsValue_ =
      new QLabel(QString::fromUtf8("Элементы: не выбраны"), sketchMirrorPanel);
  sketchMirrorElementsValue_->setObjectName(
      QStringLiteral("sketchMirrorElementsValue"));
  sketchMirrorLayout->addWidget(sketchMirrorElementsValue_);

  auto* sketchMirrorAxisRow = new QHBoxLayout;
  auto* selectAxis =
      new QPushButton(QString::fromUtf8("Выбрать ось"), sketchMirrorPanel);
  selectAxis->setObjectName(QStringLiteral("sketchMirrorSelectAxisButton"));
  auto* clearAxis =
      new QPushButton(QString::fromUtf8("Сбросить"), sketchMirrorPanel);
  clearAxis->setObjectName(QStringLiteral("sketchMirrorClearAxisButton"));
  sketchMirrorAxisRow->addWidget(selectAxis, 1);
  sketchMirrorAxisRow->addWidget(clearAxis);
  sketchMirrorLayout->addLayout(sketchMirrorAxisRow);
  sketchMirrorAxisValue_ =
      new QLabel(QString::fromUtf8("Ось: не выбрана"), sketchMirrorPanel);
  sketchMirrorAxisValue_->setObjectName(QStringLiteral("sketchMirrorAxisValue"));
  sketchMirrorLayout->addWidget(sketchMirrorAxisValue_);

  sketchMirrorApplyButton_ =
      new QPushButton(QString::fromUtf8("Создать зеркало"), sketchMirrorPanel);
  sketchMirrorApplyButton_->setObjectName(
      QStringLiteral("sketchMirrorApplyButton"));
  sketchMirrorApplyButton_->setEnabled(false);
  sketchMirrorLayout->addWidget(sketchMirrorApplyButton_);
  auto* closeSketchMirror =
      new QPushButton(QString::fromUtf8("Закрыть"), sketchMirrorPanel);
  closeSketchMirror->setObjectName(QStringLiteral("sketchMirrorCancelButton"));
  sketchMirrorLayout->addWidget(closeSketchMirror);
  sketchMirrorLayout->addStretch();

  sketchMirrorDock_->setWidget(sketchMirrorPanel);
  sketchMirrorDock_->setMinimumWidth(270);
  addDockWidget(Qt::LeftDockWidgetArea, sketchMirrorDock_);
  sketchMirrorDock_->hide();

  connect(selectElements, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    sketchCanvas_->beginMirrorElementSelection();
  });
  connect(clearElements, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    sketchCanvas_->clearMirrorElementSelection();
  });
  connect(selectAxis, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    sketchCanvas_->beginMirrorAxisSelection();
  });
  connect(clearAxis, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    sketchCanvas_->clearMirrorAxisSelection();
  });
  connect(sketchMirrorApplyButton_, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    sketchCanvas_->applyMirrorSelection();
  });
  connect(closeSketchMirror, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    sketchCanvas_->clearMirrorElementSelection();
    sketchCanvas_->setTool(SketchCanvas::Tool::Select);
  });
  connect(sketchCanvas_, &SketchCanvas::mirrorSelectionChanged, this,
          [this](int elementCount, bool axisSelected, bool selectingAxis) {
            if (!partProjectActive()) return;
            sketchMirrorElementsValue_->setText(
                elementCount > 0
                    ? QString::fromUtf8("Элементы: выбрано %1")
                          .arg(elementCount)
                    : QString::fromUtf8("Элементы: не выбраны"));
            sketchMirrorAxisValue_->setText(
                axisSelected
                    ? QString::fromUtf8("Ось: выбрана")
                    : selectingAxis
                          ? QString::fromUtf8("Ось: выберите на холсте")
                          : QString::fromUtf8("Ось: не выбрана"));
            sketchMirrorApplyButton_->setEnabled(elementCount > 0 &&
                                                 axisSelected);
          });
  connect(gridCheck, &QCheckBox::toggled, this, [this](bool visible) {
    if (!partProjectActive()) return;
    sketchCanvas_->setGridVisible(visible);
  });
  connect(snapCheck, &QCheckBox::toggled, this, [this](bool enabled) {
    if (!partProjectActive()) return;
    sketchCanvas_->setSnapEnabled(enabled);
  });
  connect(orientSketchPlane, &QPushButton::clicked, this, [this] {
    if (!partProjectActive()) return;
    sketchCanvas_->resetViewRotation();
  });
  connect(sketchLineTypeCombo_, &QComboBox::currentIndexChanged, this,
          [this](int index) {
            if (!partProjectActive()) return;
            sketchCanvas_->setSelectedDashed(index == 1);
          });
  connect(circleDiameterSpin, &QDoubleSpinBox::valueChanged, this,
          [this](double value) {
            if (!partProjectActive()) return;
            sketchCanvas_->setCircleDiameter(value);
          });
  connect(circleModeGroup, &QButtonGroup::idClicked, this,
          [this](int index) {
            if (!partProjectActive()) return;
            sketchCanvas_->setCircleMode(
                static_cast<SketchCanvas::CircleMode>(index));
          });
  connect(rectangleModeGroup, &QButtonGroup::idClicked, this,
          [this](int index) {
            if (!partProjectActive()) return;
            sketchCanvas_->setRectangleMode(
                static_cast<SketchCanvas::RectangleMode>(index));
          });
  connect(sketchCanvas_, &SketchCanvas::primaryDimensionChanged, this,
          [this, circleDiameterSpin](double value) {
            if (!partProjectActive()) return;
            if (sketchCanvas_->tool() != SketchCanvas::Tool::Circle) return;
            const QSignalBlocker blocker(circleDiameterSpin);
            circleDiameterSpin->setValue(value);
          });
  connect(sketchCanvas_, &SketchCanvas::selectionChanged, this,
          [this](const QString&) {
            if (!partProjectActive()) return;
            updateSketchConstraintPanel();
          });
  connect(sketchCanvas_, &SketchCanvas::geometryChanged, this,
          [this](double, double) {
            if (!partProjectActive()) return;
            updateSketchConstraintPanel();
          });
  connect(sketchConstraintsList_, &QTreeWidget::itemChanged, this,
          [this](QTreeWidgetItem* item, int column) {
            if (!partProjectActive()) return;
            if (!item || column != 1) return;

            const auto dimensionIndexValue =
                item->data(0, Qt::UserRole + 1).toULongLong();
            const bool isDimension =
                dimensionIndexValue != std::numeric_limits<qulonglong>::max();

            const auto mode = item->checkState(1) == Qt::Checked
                                  ? sketch::ConstraintMode::Driving
                              : item->checkState(1) == Qt::PartiallyChecked
                                  ? sketch::ConstraintMode::Reference
                                  : sketch::ConstraintMode::Disabled;

            const QVariant idData = item->data(0, Qt::UserRole);
            if (!idData.isValid()) return;
            const auto id =
                static_cast<sketch::ConstraintId>(idData.toULongLong());

            if (isDimension) {
              const auto dimensionIndex =
                  static_cast<std::size_t>(dimensionIndexValue);
              const std::uint64_t documentGeneration =
                  previewDocumentGeneration_;

              QTimer::singleShot(0, this,
                                 [this, documentGeneration, dimensionIndex, id,
                                  mode] {
                if (!partProjectActive()) return;
                if (documentGeneration != previewDocumentGeneration_) return;
                if (!sketchCanvas_) return;
                if (mode == sketch::ConstraintMode::Disabled)
                  sketchCanvas_->setConstraintModeById(id, mode);
                else
                  sketchCanvas_->setDimensionDriving(
                      dimensionIndex,
                      mode == sketch::ConstraintMode::Driving);
              });
              return;
            }

            const std::uint64_t documentGeneration =
                previewDocumentGeneration_;
            QTimer::singleShot(0, this, [this, documentGeneration, id, mode] {
              if (!partProjectActive()) return;
              if (documentGeneration != previewDocumentGeneration_) return;
              if (sketchCanvas_)
                sketchCanvas_->setConstraintModeById(id, mode);
            });
          });
  connect(sketchCanvas_, &SketchCanvas::lineStyleSelectionChanged, this,
          [this](bool elementSelected, bool dashed) {
            if (!partProjectActive()) return;
            const QSignalBlocker blocker(sketchLineTypeCombo_);
            sketchLineTypeCombo_->setEnabled(elementSelected);
            sketchLineTypeCombo_->setCurrentIndex(dashed ? 1 : 0);
            if (elementSelected) {
              sketchSettingsDock_->show();
              sketchSettingsDock_->raise();
            }
          });
  connect(sketchCanvas_, &SketchCanvas::toolChanged, this,
          [this, circlePropertiesSection,
           rectanglePropertiesSection](SketchCanvas::Tool tool) {
            if (!partProjectActive()) return;
            circlePropertiesSection->setVisible(
                tool == SketchCanvas::Tool::Circle);
            rectanglePropertiesSection->setVisible(
                tool == SketchCanvas::Tool::Rectangle);
            if (sketchMirrorDock_) {
              sketchMirrorDock_->setVisible(tool == SketchCanvas::Tool::Mirror);
              if (tool == SketchCanvas::Tool::Mirror)
                sketchMirrorDock_->raise();
            }
            if (tool != SketchCanvas::Tool::Select) {
              sketchLineTypeCombo_->setEnabled(false);
              sketchSettingsDock_->show();
              sketchSettingsDock_->raise();
            }
          });

  sketchRibbon_ = new SketchRibbon(sketchCanvas_, this);
  modelRibbon_ = new ModelRibbon(this);
  ribbonStack_ = new QStackedWidget(this);
  ribbonStack_->addWidget(modelRibbon_);
  ribbonStack_->addWidget(sketchRibbon_);
  assemblyRibbon_ = new QWidget(ribbonStack_);
  assemblyRibbon_->setObjectName(QStringLiteral("assemblyReadOnlyRibbon"));
  auto* assemblyRibbonLayout = new QHBoxLayout(assemblyRibbon_);
  assemblyRibbonLayout->setContentsMargins(18, 8, 18, 8);
  auto* assemblyRibbonLabel = new QLabel(
      QString::fromUtf8("СБОРКА"),
      assemblyRibbon_);
  assemblyRibbonLabel->setProperty("uiRole", "secondaryText");
  assemblyRibbonLayout->addWidget(assemblyRibbonLabel);
  assemblyNewButton_ = new QPushButton(QString::fromUtf8("Новая сборка"),
                                       assemblyRibbon_);
  assemblyNewButton_->setObjectName(QStringLiteral("assemblyNewCommand"));
  assemblyInsertButton_ = new QPushButton(
      QString::fromUtf8("Вставить компонент"), assemblyRibbon_);
  assemblyInsertButton_->setObjectName(
      QStringLiteral("assemblyInsertCommand"));
  assemblyMateButton_ = new QPushButton(
      QString::fromUtf8("Сопряжение"), assemblyRibbon_);
  assemblyMateButton_->setObjectName(QStringLiteral("assemblyMateCommand"));
  assemblyRenameAction_ = new QAction(
      QString::fromUtf8("Переименовать экземпляр…"), this);
  assemblyRenameAction_->setObjectName(
      QStringLiteral("assemblyRenameOccurrenceAction"));
  assemblyRenameAction_->setEnabled(false);
  assemblyRenameButton_ = new QPushButton(
      assemblyRenameAction_->text(), assemblyRibbon_);
  assemblyRenameButton_->setObjectName(
      QStringLiteral("assemblyRenameOccurrenceCommand"));
  assemblyRenameButton_->setEnabled(false);
  assemblyVisibilityAction_ = new QAction(
      QString::fromUtf8("Скрыть экземпляр"), this);
  assemblyVisibilityAction_->setObjectName(
      QStringLiteral("assemblyVisibilityOccurrenceAction"));
  assemblyVisibilityAction_->setEnabled(false);
  assemblyVisibilityButton_ = new QPushButton(
      assemblyVisibilityAction_->text(), assemblyRibbon_);
  assemblyVisibilityButton_->setObjectName(
      QStringLiteral("assemblyVisibilityOccurrenceCommand"));
  assemblyVisibilityButton_->setEnabled(false);
  assemblyGroundingAction_ = new QAction(
      QString::fromUtf8("Зафиксировать экземпляр"), this);
  assemblyGroundingAction_->setObjectName(
      QStringLiteral("assemblyGroundingOccurrenceAction"));
  assemblyGroundingAction_->setEnabled(false);
  assemblyGroundingButton_ = new QPushButton(
      assemblyGroundingAction_->text(), assemblyRibbon_);
  assemblyGroundingButton_->setObjectName(
      QStringLiteral("assemblyGroundingOccurrenceCommand"));
  assemblyGroundingButton_->setEnabled(false);
  assemblySuppressionAction_ = new QAction(
      QString::fromUtf8("Подавить экземпляр"), this);
  assemblySuppressionAction_->setObjectName(
      QStringLiteral("assemblySuppressionOccurrenceAction"));
  assemblySuppressionAction_->setEnabled(false);
  assemblySuppressionButton_ = new QPushButton(
      assemblySuppressionAction_->text(), assemblyRibbon_);
  assemblySuppressionButton_->setObjectName(
      QStringLiteral("assemblySuppressionOccurrenceCommand"));
  assemblySuppressionButton_->setEnabled(false);
  assemblyDuplicateAction_ = new QAction(
      QString::fromUtf8("Дублировать экземпляр"), this);
  assemblyDuplicateAction_->setObjectName(
      QStringLiteral("assemblyDuplicateOccurrenceAction"));
  assemblyDuplicateAction_->setEnabled(false);
  assemblyDuplicateButton_ = new QPushButton(
      assemblyDuplicateAction_->text(), assemblyRibbon_);
  assemblyDuplicateButton_->setObjectName(
      QStringLiteral("assemblyDuplicateOccurrenceCommand"));
  assemblyDuplicateButton_->setEnabled(false);
  assemblyRemoveAction_ = new QAction(
      QString::fromUtf8("Удалить экземпляр"), this);
  assemblyRemoveAction_->setObjectName(
      QStringLiteral("assemblyRemoveOccurrenceAction"));
  assemblyRemoveAction_->setEnabled(false);
  assemblyRemoveButton_ = new QPushButton(
      assemblyRemoveAction_->text(), assemblyRibbon_);
  assemblyRemoveButton_->setObjectName(
      QStringLiteral("assemblyRemoveOccurrenceCommand"));
  assemblyRemoveButton_->setEnabled(false);
  assemblyEditDefinitionAction_ = new QAction(
      QString::fromUtf8("Редактировать деталь"), this);
  assemblyEditDefinitionAction_->setObjectName(
      QStringLiteral("assemblyEditDefinitionAction"));
  assemblyEditDefinitionAction_->setEnabled(false);
  assemblyEditDefinitionButton_ = new QPushButton(
      assemblyEditDefinitionAction_->text(), assemblyRibbon_);
  assemblyEditDefinitionButton_->setObjectName(
      QStringLiteral("assemblyEditDefinitionCommand"));
  assemblyEditDefinitionButton_->setEnabled(false);
  assemblyIsolationAction_ = new QAction(
      QString::fromUtf8("Изолировать экземпляр"), this);
  assemblyIsolationAction_->setObjectName(
      QStringLiteral("assemblyIsolationOccurrenceAction"));
  assemblyIsolationAction_->setEnabled(false);
  assemblyIsolationButton_ = new QPushButton(
      assemblyIsolationAction_->text(), assemblyRibbon_);
  assemblyIsolationButton_->setObjectName(
      QStringLiteral("assemblyIsolationOccurrenceCommand"));
  assemblyIsolationButton_->setEnabled(false);
  auto* assemblyCommandColumn = new QVBoxLayout;
  assemblyCommandColumn->setSpacing(4);
  assemblyCommandColumn->addWidget(assemblyNewButton_);
  assemblyCommandColumn->addWidget(assemblyInsertButton_);
  assemblyCommandColumn->addWidget(assemblyMateButton_);
  assemblyCommandColumn->addWidget(assemblyRenameButton_);
  assemblyCommandColumn->addWidget(assemblyVisibilityButton_);
  assemblyCommandColumn->addWidget(assemblyGroundingButton_);
  assemblyCommandColumn->addWidget(assemblySuppressionButton_);
  assemblyCommandColumn->addWidget(assemblyDuplicateButton_);
  assemblyCommandColumn->addWidget(assemblyRemoveButton_);
  assemblyCommandColumn->addWidget(assemblyEditDefinitionButton_);
  assemblyCommandColumn->addWidget(assemblyIsolationButton_);
  assemblyRibbonLayout->addLayout(assemblyCommandColumn);
  auto* assemblyPlacementPanel = new QWidget(assemblyRibbon_);
  auto* assemblyPlacementGrid = new QGridLayout(assemblyPlacementPanel);
  assemblyPlacementGrid->setContentsMargins(0, 0, 0, 0);
  assemblyPlacementGrid->setHorizontalSpacing(5);
  assemblyPlacementGrid->setVerticalSpacing(3);
  const std::array<const char*, 3> translationNames{"X", "Y", "Z"};
  const std::array<const char*, 3> rotationNames{"Rx", "Ry", "Rz"};
  for (std::size_t i = 0; i < 3; ++i) {
    auto* label = new QLabel(QString::fromLatin1(translationNames[i]),
                             assemblyRibbon_);
    assemblyTranslationSpins_[i] = new QDoubleSpinBox(assemblyRibbon_);
    assemblyTranslationSpins_[i]->setObjectName(
        QStringLiteral("assemblyTranslation%1").arg(i));
    assemblyTranslationSpins_[i]->setRange(-100000.0, 100000.0);
    assemblyTranslationSpins_[i]->setDecimals(3);
    assemblyTranslationSpins_[i]->setEnabled(false);
    assemblyTranslationSpins_[i]->setMaximumWidth(76);
    assemblyPlacementGrid->addWidget(label, 0, static_cast<int>(i) * 2);
    assemblyPlacementGrid->addWidget(assemblyTranslationSpins_[i], 0,
                                     static_cast<int>(i) * 2 + 1);
  }
  for (std::size_t i = 0; i < 3; ++i) {
    auto* label = new QLabel(QString::fromLatin1(rotationNames[i]),
                             assemblyRibbon_);
    assemblyRotationSpins_[i] = new QDoubleSpinBox(assemblyRibbon_);
    assemblyRotationSpins_[i]->setObjectName(
        QStringLiteral("assemblyRotation%1").arg(i));
    assemblyRotationSpins_[i]->setRange(-36000.0, 36000.0);
    assemblyRotationSpins_[i]->setDecimals(2);
    assemblyRotationSpins_[i]->setEnabled(false);
    assemblyRotationSpins_[i]->setMaximumWidth(72);
    assemblyPlacementGrid->addWidget(label, 1, static_cast<int>(i) * 2);
    assemblyPlacementGrid->addWidget(assemblyRotationSpins_[i], 1,
                                     static_cast<int>(i) * 2 + 1);
  }
  assemblyRibbonLayout->addWidget(assemblyPlacementPanel);
  assemblyMateOpposedCheck_ = new QCheckBox(
      QString::fromUtf8("Противоположно"), assemblyRibbon_);
  assemblyMateOpposedCheck_->setObjectName(
      QStringLiteral("assemblyMateOpposed"));
  assemblyMateOpposedCheck_->hide();
  assemblyMateStatusLabel_ = new QLabel(assemblyRibbon_);
  assemblyMateStatusLabel_->setObjectName(
      QStringLiteral("assemblyMateStatus"));
  assemblyMateStatusLabel_->setProperty("uiRole", "secondaryText");
  assemblyMateStatusLabel_->hide();
  auto* assemblyMateColumn = new QVBoxLayout;
  assemblyMateColumn->addWidget(assemblyMateOpposedCheck_);
  assemblyMateColumn->addWidget(assemblyMateStatusLabel_);
  assemblyRibbonLayout->addLayout(assemblyMateColumn);
  assemblyApplyButton_ = new QPushButton(QString::fromUtf8("Применить"),
                                         assemblyRibbon_);
  assemblyApplyButton_->setObjectName(QStringLiteral("assemblyApplyCommand"));
  assemblyApplyButton_->setEnabled(false);
  assemblyCancelButton_ = new QPushButton(QString::fromUtf8("Отмена"),
                                          assemblyRibbon_);
  assemblyCancelButton_->setObjectName(
      QStringLiteral("assemblyCancelCommand"));
  assemblyCancelButton_->setEnabled(false);
  auto* assemblyCommitColumn = new QVBoxLayout;
  assemblyCommitColumn->setSpacing(4);
  assemblyCommitColumn->addWidget(assemblyApplyButton_);
  assemblyCommitColumn->addWidget(assemblyCancelButton_);
  assemblyRibbonLayout->addLayout(assemblyCommitColumn);
  connect(assemblyNewButton_, &QPushButton::clicked, this,
          &MainWindow::createNewAssembly);
  connect(assemblyInsertButton_, &QPushButton::clicked, this,
          &MainWindow::beginAssemblyInsert);
  connect(assemblyMateButton_, &QPushButton::clicked, this,
          &MainWindow::beginAssemblyMate);
  connect(viewport_, &Viewport::assemblyMateFacePicked, this,
          &MainWindow::acceptAssemblyMatePick, Qt::DirectConnection);
  connect(assemblyMateOpposedCheck_, &QCheckBox::toggled, this,
          [this](bool opposed) {
            if (!assemblyMateActive()) return;
            QPointer<MainWindow> receiver(this);
            const auto incarnation = productDocument_.incarnation();
            const auto root = productDocument_.rootDefinitionId();
            const auto publication =
                viewport_ && viewport_->assemblySession()
                    ? viewport_->assemblySession()->publicationHandle()
                    : AssemblyScenePublicationHandle{};
            static_cast<void>(assemblyMateController_->session().setOrientation(
                productDocument_, opposed ? PlanarMateOrientation::Opposed
                                          : PlanarMateOrientation::Aligned));
            if (!receiver || !assemblyMateActive() || !viewport_ ||
                !viewport_->assemblySession() ||
                productDocument_.incarnation() != incarnation ||
                productDocument_.rootDefinitionId() != root ||
                !viewport_->assemblySession()
                     ->publicationHandle()
                     .isSameCurrentPublication(publication))
              return;
            const auto revision =
                assemblyMateController_->session().callbackRevision();
            if (assemblyMateController_->session().currentPreview())
              static_cast<void>(
                  presentCurrentAssemblyMatePreview(revision));
            updateAssemblyMateUi();
          });
  connect(assemblyRenameAction_, &QAction::triggered, this,
          &MainWindow::beginAssemblyRename);
  connect(assemblyRenameButton_, &QPushButton::clicked,
          assemblyRenameAction_, &QAction::trigger);
  connect(assemblyVisibilityAction_, &QAction::triggered, this,
          &MainWindow::toggleAssemblyOccurrenceVisibility);
  connect(assemblyVisibilityButton_, &QPushButton::clicked,
          assemblyVisibilityAction_, &QAction::trigger);
  connect(assemblyGroundingAction_, &QAction::triggered, this,
          &MainWindow::toggleAssemblyOccurrenceGrounded);
  connect(assemblyGroundingButton_, &QPushButton::clicked,
          assemblyGroundingAction_, &QAction::trigger);
  connect(assemblySuppressionAction_, &QAction::triggered, this,
          &MainWindow::toggleAssemblyOccurrenceSuppressed);
  connect(assemblySuppressionButton_, &QPushButton::clicked,
          assemblySuppressionAction_, &QAction::trigger);
  connect(assemblyDuplicateAction_, &QAction::triggered, this,
          &MainWindow::duplicateAssemblyOccurrence);
  connect(assemblyDuplicateButton_, &QPushButton::clicked,
          assemblyDuplicateAction_, &QAction::trigger);
  connect(assemblyRemoveAction_, &QAction::triggered, this,
          &MainWindow::removeAssemblyOccurrence);
  connect(assemblyRemoveButton_, &QPushButton::clicked,
          assemblyRemoveAction_, &QAction::trigger);
  connect(assemblyEditDefinitionAction_, &QAction::triggered, this,
          &MainWindow::enterAssemblyDefinitionEdit);
  connect(assemblyEditDefinitionButton_, &QPushButton::clicked,
          assemblyEditDefinitionAction_, &QAction::trigger);
  connect(assemblyIsolationAction_, &QAction::triggered, this,
          &MainWindow::toggleAssemblyIsolation);
  connect(assemblyIsolationButton_, &QPushButton::clicked,
          assemblyIsolationAction_, &QAction::trigger);
  connect(assemblyApplyButton_, &QPushButton::clicked, this, [this] {
    if (assemblyMateActive())
      applyAssemblyMate();
    else
      applyAssemblyInsert();
  });
  connect(assemblyCancelButton_, &QPushButton::clicked, this, [this] {
    if (assemblyMateActive())
      cancelAssemblyMate();
    else
      cancelAssemblyInsert();
  });
  for (auto* spin : assemblyTranslationSpins_)
    connect(spin, &QDoubleSpinBox::valueChanged, this,
            [this](double) { updateAssemblyInsertPlacement(); });
  for (auto* spin : assemblyRotationSpins_)
    connect(spin, &QDoubleSpinBox::valueChanged, this,
            [this](double) { updateAssemblyInsertPlacement(); });
  assemblyInsertEscapeShortcut_ =
      new QShortcut(QKeySequence(Qt::Key_Escape), this);
  assemblyInsertEscapeShortcut_->setObjectName(
      QStringLiteral("assemblyInsertEscapeShortcut"));
  assemblyInsertEscapeShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
  connect(assemblyInsertEscapeShortcut_, &QShortcut::activated, this, [this] {
    if (partProjectActive()) return;
    if (assemblyMateActive()) {
      assemblyMateController_->session().escape();
      if (!assemblyMateActive()) {
        cancelAssemblyMate();
      } else {
        if (viewport_) viewport_->clearAssemblyMatePreview();
        updateAssemblyMateUi();
      }
      return;
    }
    if (assemblyInsertController_.active()) {
      if (!assemblyInsertController_.handleEscape()) cancelAssemblyInsert();
      return;
    }
    if (viewport_ && viewport_->assemblySession() &&
        viewport_->assemblySession()->isolationActive())
      toggleAssemblyIsolation();
  });
  assemblyRenameShortcut_ = new QShortcut(QKeySequence(Qt::Key_F2), this);
  assemblyRenameShortcut_->setObjectName(
      QStringLiteral("assemblyRenameOccurrenceShortcut"));
  assemblyRenameShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
  assemblyRenameShortcut_->setEnabled(false);
  connect(assemblyRenameShortcut_, &QShortcut::activated,
          assemblyRenameAction_, &QAction::trigger);
  assemblyVisibilityShortcut_ =
      new QShortcut(QKeySequence(Qt::Key_Space), this);
  assemblyVisibilityShortcut_->setObjectName(
      QStringLiteral("assemblyVisibilityOccurrenceShortcut"));
  assemblyVisibilityShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
  assemblyVisibilityShortcut_->setEnabled(false);
  connect(assemblyVisibilityShortcut_, &QShortcut::activated,
          assemblyVisibilityAction_, &QAction::trigger);
  assemblyGroundingShortcut_ =
      new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this);
  assemblyGroundingShortcut_->setObjectName(
      QStringLiteral("assemblyGroundingOccurrenceShortcut"));
  assemblyGroundingShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
  assemblyGroundingShortcut_->setEnabled(false);
  connect(assemblyGroundingShortcut_, &QShortcut::activated,
          assemblyGroundingAction_, &QAction::trigger);
  // Space is the authored visibility command; Shift+Space remains a distinct
  // exact key sequence in this window and is reserved for suppression.
  assemblySuppressionShortcut_ =
      new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Space), this);
  assemblySuppressionShortcut_->setObjectName(
      QStringLiteral("assemblySuppressionOccurrenceShortcut"));
  assemblySuppressionShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
  assemblySuppressionShortcut_->setEnabled(false);
  connect(assemblySuppressionShortcut_, &QShortcut::activated,
          assemblySuppressionAction_, &QAction::trigger);
  assemblyDuplicateShortcut_ =
      new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), this);
  assemblyDuplicateShortcut_->setObjectName(
      QStringLiteral("assemblyDuplicateOccurrenceShortcut"));
  assemblyDuplicateShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
  assemblyDuplicateShortcut_->setEnabled(false);
  connect(assemblyDuplicateShortcut_, &QShortcut::activated,
          assemblyDuplicateAction_, &QAction::trigger);
  assemblyRibbonLayout->addStretch();
  auto* assemblyDisplayMode = new QToolButton(assemblyRibbon_);
  assemblyDisplayMode->setObjectName(
      QStringLiteral("assemblyDisplayModeCommand"));
  assemblyDisplayMode->setText(QString::fromUtf8("Затенённый\nс рёбрами"));
  assemblyDisplayMode->setToolTip(
      QString::fromUtf8("Режим отображения сборки"));
  assemblyDisplayMode->setPopupMode(QToolButton::InstantPopup);
  assemblyDisplayMode->setToolButtonStyle(Qt::ToolButtonTextOnly);
  assemblyDisplayMode->setMinimumSize(112, 52);
  auto* assemblyDisplayMenu = new QMenu(assemblyDisplayMode);
  const auto addAssemblyDisplayMode =
      [this, assemblyDisplayMode, assemblyDisplayMenu](const QString& text,
                                                       int mode) {
        QAction* action = assemblyDisplayMenu->addAction(text);
        connect(action, &QAction::triggered, this,
                [this, assemblyDisplayMode, text, mode] {
                  assemblyDisplayMode->setText(text);
                  viewport_->setDisplayMode(
                      static_cast<ViewportDisplayMode>(mode));
                });
      };
  addAssemblyDisplayMode(QString::fromUtf8("Затенённый"), 0);
  addAssemblyDisplayMode(QString::fromUtf8("Затенённый с рёбрами"), 1);
  addAssemblyDisplayMode(QString::fromUtf8("Каркас"), 2);
  assemblyDisplayMode->setMenu(assemblyDisplayMenu);
  assemblyRibbonLayout->addWidget(assemblyDisplayMode);
  ribbonStack_->addWidget(assemblyRibbon_);
  const auto syncRibbonStackHeight = [this](int index) {
    if (!ribbonStack_) return;
    const QWidget* page = ribbonStack_->widget(index);
    if (!page) return;
    const int pageHeight = page->sizeHint().height();
    if (pageHeight > 0) ribbonStack_->setFixedHeight(pageHeight);
  };
  connect(ribbonStack_, &QStackedWidget::currentChanged, this,
          syncRibbonStackHeight);
  syncRibbonStackHeight(ribbonStack_->currentIndex());
  definitionEditBreadcrumb_ = new QLabel(this);
  definitionEditBreadcrumb_->setObjectName(
      QStringLiteral("assemblyDefinitionEditBreadcrumb"));
  definitionEditBreadcrumb_->setProperty("uiRole", "secondaryText");
  definitionEditBreadcrumb_->hide();
  definitionEditBackButton_ = new QPushButton(
      QString::fromUtf8("Вернуться в сборку"), this);
  definitionEditBackButton_->setObjectName(
      QStringLiteral("assemblyDefinitionEditBackCommand"));
  definitionEditBackButton_->hide();
  statusBar()->addPermanentWidget(definitionEditBreadcrumb_, 1);
  statusBar()->addPermanentWidget(definitionEditBackButton_);
  connect(definitionEditBackButton_, &QPushButton::clicked, this,
          &MainWindow::leaveAssemblyDefinitionEdit);
  connect(modelRibbon_, &ModelRibbon::createSketchRequested, this,
          [this] {
            if (!partProjectActive()) return;
            if (!ensureHistoryAtEnd()) {
              modelRibbon_->clearActiveTool();
              return;
            }
            resetTransientModelingUi();
            editingSketchIndex_.reset();
            statusBar()->showMessage(
                QString::fromUtf8("Выберите базовую плоскость или грань тела"));
            for (auto* item : featureTree_->findItems(
                     QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive)) {
              const int kind = item->data(0, Qt::UserRole).toInt();
              if (kind >= 10 && kind <= 12) item->setCheckState(0, Qt::Checked);
            }
            viewport_->beginSketchPlaneSelection();
          });
  connect(modelRibbon_, &ModelRibbon::referenceImageRequested, this,
          &MainWindow::importReferenceImage);
  connect(modelRibbon_, &ModelRibbon::extrudeRequested, this,
          [this] {
            if (!partProjectActive()) return;
            if (!ensureHistoryAtEnd()) {
              modelRibbon_->clearActiveTool();
              return;
            }
            resetTransientModelingUi();
            extrudeOperationManuallyChanged_ = false;
            extrusionReverseCheck_->setChecked(false);
            const QSignalBlocker blocker(extrusionOperationCombo_);
            extrusionOperationCombo_->setCurrentIndex(0);
            statusBar()->showMessage(
                QString::fromUtf8("Выберите замкнутый контур или грань тела"));
            viewport_->beginExtrusionSurfaceSelection();
          });
  connect(modelRibbon_, &ModelRibbon::pocketRequested, this,
          [this] {
            if (!partProjectActive()) return;
            if (!ensureHistoryAtEnd()) {
              modelRibbon_->clearActiveTool();
              return;
            }
            if (!activePartDocument().activeBody()) {
              QMessageBox::information(this, QString::fromUtf8("Вырезать"),
                  QString::fromUtf8("Сначала создайте Body."));
              return;
            }
            resetTransientModelingUi();
            extrudeOperationManuallyChanged_ = true;
            extrusionOperationCombo_->setCurrentIndex(2);
            extrusionReverseCheck_->setChecked(true);
            statusBar()->showMessage(
                QString::fromUtf8("Выберите профиль для вырезания"));
            viewport_->beginExtrusionSurfaceSelection();
          });
  connect(modelRibbon_, &ModelRibbon::revolveRequested, this,
          &MainWindow::createRevolve);
  connect(modelRibbon_, &ModelRibbon::filletRequested, this,
          &MainWindow::createFillet);
  connect(modelRibbon_, &ModelRibbon::chamferRequested, this,
          &MainWindow::createChamfer);
  connect(modelRibbon_, &ModelRibbon::joinBodiesRequested, this,
          &MainWindow::createJoinBodies);
  connect(modelRibbon_, &ModelRibbon::moveRequested, this,
          &MainWindow::createMove);
  connect(modelRibbon_, &ModelRibbon::shellRequested, this,
          &MainWindow::createShell);
  connect(modelRibbon_, &ModelRibbon::draftRequested, this,
          &MainWindow::createDraft);
  connect(modelRibbon_, &ModelRibbon::mirrorRequested, this,
          &MainWindow::createMirror);
  connect(modelRibbon_, &ModelRibbon::linearPatternRequested, this,
          &MainWindow::createLinearPattern);
  connect(modelRibbon_, &ModelRibbon::circularPatternRequested, this,
          &MainWindow::createCircularPattern);
  connect(modelRibbon_, &ModelRibbon::createComponentFromBodyRequested, this,
          &MainWindow::createComponentFromSelectedBody);
  connect(modelRibbon_, &ModelRibbon::setComponentHomePlacementRequested,
          this, [this] {
            applyComponentHomePlacement(
                ComponentHomePlacementOperation::AdoptCurrent);
          });
  connect(modelRibbon_, &ModelRibbon::restoreComponentHomePlacementRequested,
          this, [this] {
            applyComponentHomePlacement(
                ComponentHomePlacementOperation::Restore);
          });
  connect(modelRibbon_, &ModelRibbon::rulerToggled, this,
          [this](bool active) {
            if (!partProjectActive()) return;
            if (active) {
              resetTransientModelingUi();
              viewport_->beginRulerMeasurement();
              viewport_->setFocus(Qt::MouseFocusReason);
              statusBar()->showMessage(
                  QString::fromUtf8("Линейка: выберите первую точку"));
            } else {
              viewport_->cancelRulerMeasurement();
              statusBar()->showMessage(
                  QString::fromUtf8("Линейка выключена"), 2000);
            }
          });
  connect(viewport_, &Viewport::rulerActiveChanged, this,
          [this](bool active) {
            if (!partProjectActive()) return;
            modelRibbon_->setRulerActive(active);
          });
  connect(viewport_, &Viewport::rulerPointPicked, this,
          [this](int selectedPointCount) {
            if (!partProjectActive()) return;
            if (selectedPointCount == 1)
              statusBar()->showMessage(
                  QString::fromUtf8("Линейка: выберите вторую точку"));
          });
  connect(viewport_, &Viewport::rulerMeasurementChanged, this,
          [this](double distanceMm) {
            if (!partProjectActive()) return;
            statusBar()->showMessage(
                QString::fromUtf8("Расстояние: %1 мм. Щёлкните ещё раз для нового измерения")
                    .arg(QLocale().toString(distanceMm, 'f', 2)));
          });
  connect(modelRibbon_, &ModelRibbon::displayModeRequested, viewport_,
          [this](int mode) {
            viewport_->setDisplayMode(static_cast<ViewportDisplayMode>(mode));
          });
  connect(modelRibbon_, &ModelRibbon::meshQualityRequested, viewport_,
          [this](int quality) {
            if (!partProjectActive()) return;
            viewport_->setMeshQuality(static_cast<ViewportMeshQuality>(quality));
          });
  connect(viewport_, &Viewport::sketchPlanePicked, this,
          [this](const SketchPlanePick& pick) {
            if (!partProjectActive()) return;
            viewport_->setSelectionFilter(SelectionFilter::Any);
            modelRibbon_->clearActiveTool();
            currentSketchSupport_ = pick.presentationLabel;
            currentSketchFaceReference_.reset();
            if (const auto* datum = std::get_if<DatumPlanePick>(&pick.source)) {
              currentSketchPlacement_ = datum->placement;
            } else if (const auto* legacy =
                           std::get_if<LegacySolidFacePick>(&pick.source)) {
              currentSketchPlacement_ = legacy->placement;
            } else if (const auto* bodyFace =
                           std::get_if<BodyFacePick>(&pick.source)) {
              const FaceReference& faceReference = bodyFace->face;
              if (faceReference.bodyId() == kInvalidBodyId ||
                  faceReference.featureId() == kInvalidFeatureId) {
                QMessageBox::warning(this, QString::fromUtf8("Sketch on Face"),
                                     QString::fromUtf8("Не удалось определить грань Body."));
                return;
              }
              const ShapeFeature* feature = mutableStandaloneDocument().findFeature(
                  faceReference.bodyId(), faceReference.featureId());
              const auto shape = feature ? feature->shape() : ShapeFeature::ShapePtr{};
              if (!shape) {
                QMessageBox::warning(
                    this, QString::fromUtf8("Sketch on Face"),
                    QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели."));
                return;
              }
              const auto topology = feature->topologyIndex();
              const auto resolvedFace = topology
                                            ? topology->resolveFace(faceReference)
                                            : FaceResolution{};
              const auto resolved = resolvedFace
                                        ? resolveFacePlacement(
                                              *resolvedFace.subshape)
                                        : ResolvedFacePlacement{};
              if (!resolved.resolved) {
                QMessageBox::warning(
                    this, QString::fromUtf8("Sketch on Face"),
                    QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели."));
                return;
              }
              if (!resolved.planar) {
                QMessageBox::information(
                    this, QString::fromUtf8("Sketch on Face"),
                    QString::fromUtf8("Создание эскиза на криволинейной поверхности пока не поддерживается."));
                return;
              }
              currentSketchPlacement_ = resolved.placement;
              currentSketchFaceReference_ = faceReference;
            }
            Vector3d targetEye = currentSketchPlacement_.normal();
            // A datum plane has no material "outside", so choose the normal
            // hemisphere already closest to the current camera. A real body
            // face does have an outside: always approach that exact side so a
            // picked front face cannot finish behind the model.
            if (std::holds_alternative<DatumPlanePick>(pick.source)) {
              const double facing = targetEye.x * pick.viewDirection.x +
                                    targetEye.y * pick.viewDirection.y +
                                    targetEye.z * pick.viewDirection.z;
              if (facing < 0.0)
                targetEye = {-targetEye.x, -targetEye.y, -targetEye.z};
            }
            const CameraOrientation targetCamera =
                uprightOrientationForDirection(
                    {targetEye.x, targetEye.y, targetEye.z},
                    viewport_->cameraYawDegrees());
            const Point3d targetUp = cameraScreenUpDirection(targetCamera);
            sketchCanvas_->resetSketch();
            sketchCanvas_->clearSketchEditContext();
            // Build the final Sketcher camera before animating the 3D view.
            // Both modes therefore agree on eye hemisphere, screen-up, scale
            // and the screen position of the placement's real world origin.
            sketchCanvas_->setInitialViewUp(
                {targetUp.x, targetUp.y, targetUp.z});
            sketchCanvas_->setInitialViewDirection(targetEye);
            sketchCanvas_->setReferenceBody(activePartDocument().box(),
                                             currentSketchPlacement_,
                                             hasDisplayableModernSolid() ||
                                                 hasHistoricalLegacyExtrusion());
            const bool historicalCap =
                std::holds_alternative<LegacySolidFacePick>(pick.source) &&
                hasHistoricalLegacyExtrusion();
            sketchCanvas_->setReferenceProfile(viewport_->solidSketch(),
                                               historicalCap);
            if (currentSketchFaceReference_ &&
                !configureSketchEditContext(true))
              return;
            configureSketchSceneReferences();
            const QString supportLabel = pick.presentationLabel;
            QPointer<MainWindow> guarded(this);
            statusBar()->showMessage(
                QString::fromUtf8("Переход к рабочей плоскости: ") +
                supportLabel);
            viewport_->animateToSketchView(
                targetCamera, {},
                sketchCanvas_->worldOriginScreenPosition(),
                sketchCanvas_->viewPixelsPerMillimeter(),
                [guarded, supportLabel] {
                  if (!guarded || !guarded->partProjectActive()) return;
                  guarded->workspaceStack_->setCurrentWidget(
                      guarded->sketchCanvas_);
                  guarded->ribbonStack_->setCurrentWidget(
                      guarded->sketchRibbon_);
                  guarded->statusBar()->showMessage(
                      QString::fromUtf8("Рабочая плоскость: ") +
                      supportLabel);
                });
          });
  connect(viewport_, &Viewport::imagePlanePicked, this,
          &MainWindow::finishReferenceImageImport);
  connect(viewport_, &Viewport::referenceImageParameterChanged, this,
          &MainWindow::updateReferenceImageParameter);
  connect(viewport_, &Viewport::extrusionSourcePicked, this,
          [this](const ExtrusionSourcePick& pick) {
            if (!partProjectActive()) return;
            if (const auto* face = std::get_if<BodyFacePick>(&pick.source)) {
              createFaceExtrude(face->face);
              return;
            }
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle != ToolLifecycle::Inactive) {
              updateRevolveProfileSelection(pick);
              return;
            }
            selectedExtrusionSource_ = pick;
            statusBar()->showMessage(QString::fromUtf8("Поверхность: ") +
                                     pick.presentationLabel);
            extrusionLengthSpin_->setValue(activePartDocument().box().heightMm);
            viewport_->showExtrusionManipulator(
                extrusionReverseCheck_->isChecked()
                    ? -extrusionLengthSpin_->value()
                    : extrusionLengthSpin_->value());
            scheduleAutomaticExtrudeOperation();
            extrusionDock_->show();
            extrusionDock_->raise();
            updateLegacyExtrusionPreview();
          });
  connect(viewport_, &Viewport::directProfilePicked, this,
          [this](const ExtrusionSourcePick& pick) {
            if (!partProjectActive()) return;
            createSketchExtrude(pick);
          });
  connect(viewport_, &Viewport::bodyMoveCommitted, this,
          [this](QPointF previous, QPointF current) {
            if (!partProjectActive()) return;
            HistorySelectionState after = captureHistorySelection();
            HistorySelectionState before = after;
            before.presentation.bodyPositionX = previous.x();
            before.presentation.bodyPositionY = previous.y();
            after.presentation.bodyPositionX = current.x();
            after.presentation.bodyPositionY = current.y();
            static_cast<void>(pushPresentationTransition(
                std::move(before), std::move(after)));
          });
  connect(sketchRibbon_, &SketchRibbon::finishRequested, this,
          &MainWindow::finishSketch);
  connect(sketchCanvas_, &SketchCanvas::geometryChanged, this,
          &MainWindow::updateFromSketch);
  connect(workspaceStack_, &QStackedWidget::currentChanged, this, [this](int index) {
    if (!modelWorkspaceActive()) {
      if (ribbonStack_) ribbonStack_->setCurrentWidget(assemblyRibbon_);
      if (sketchSettingsDock_) sketchSettingsDock_->hide();
      if (sketchMirrorDock_) sketchMirrorDock_->hide();
      return;
    }
    const bool sketchMode = workspaceStack_->widget(index) == sketchCanvas_;
    ribbonStack_->setCurrentWidget(sketchMode
                                       ? static_cast<QWidget*>(sketchRibbon_)
                                       : static_cast<QWidget*>(modelRibbon_));
    if (sketchSettingsDock_) {
      sketchSettingsDock_->setVisible(sketchMode);
      if (sketchMode) sketchSettingsDock_->raise();
    }
    if (sketchMirrorDock_) {
      const bool mirrorVisible =
          sketchMode && sketchCanvas_->tool() == SketchCanvas::Tool::Mirror;
      sketchMirrorDock_->setVisible(mirrorVisible);
      if (mirrorVisible) sketchMirrorDock_->raise();
    }
    if (auto* modelTreeDock =
            findChild<QDockWidget*>(QStringLiteral("modelTreeDock"))) {
      modelTreeDock->setVisible(!sketchMode);
      if (!sketchMode) modelTreeDock->raise();
    }
    if (sketchMode && viewport_->referenceImageManipulatorActive())
      viewport_->clearToolManipulator();
    else if (!sketchMode)
      showSelectedReferenceImageManipulator();
    updateUndoAvailability();
  });
  workspaceStack_->setCurrentWidget(viewport_);
  ribbonStack_->setCurrentWidget(modelRibbon_);

  modelTreeDock_ =
      new QDockWidget(QString::fromUtf8("Дерево построений"), this);
  modelTreeDock_->setObjectName(QStringLiteral("modelTreeDock"));
  featureTree_ = new QTreeWidget(modelTreeDock_);
  featureTree_->setHeaderHidden(true);
  featureTree_->setAlternatingRowColors(true);
  featureTree_->setContextMenuPolicy(Qt::CustomContextMenu);
  rebuildFeatureTree();
  connect(featureTree_, &QTreeWidget::customContextMenuRequested, this,
          [this](const QPoint& point) {
            if (!partProjectActive()) return;
            auto* item = featureTree_->itemAt(point);
            if (!item) return;
            const int kind = item->data(0, Qt::UserRole).toInt();
            if (kind == 4) {
              const auto imageId = static_cast<ReferenceImageId>(
                  item->data(0, Qt::UserRole + 3).toULongLong());
              QMenu menu(featureTree_);
              QAction* remove =
                  menu.addAction(QString::fromUtf8("Удалить изображение"));
              if (menu.exec(featureTree_->viewport()->mapToGlobal(point)) ==
                  remove)
                removeReferenceImage(imageId);
              return;
            }
            const auto bodyId = static_cast<BodyId>(
                item->data(0, Qt::UserRole + 2).toULongLong());
            if (bodyId == kInvalidBodyId) return;
            QMenu menu(featureTree_);
            QAction* remove = menu.addAction(QString::fromUtf8("Удалить Body"));
            if (menu.exec(featureTree_->viewport()->mapToGlobal(point)) == remove)
              removeBody(bodyId);
          });
  auto* deleteBodyShortcut =
      new QShortcut(QKeySequence(Qt::Key_Delete), featureTree_);
  deleteBodyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(deleteBodyShortcut, &QShortcut::activated, this, [this] {
    if (!partProjectActive()) return;
    auto* item = featureTree_->currentItem();
    if (!item) return;
    if (item->data(0, Qt::UserRole).toInt() == 4) {
      removeReferenceImage(static_cast<ReferenceImageId>(
          item->data(0, Qt::UserRole + 3).toULongLong()));
      return;
    }
    const auto bodyId = static_cast<BodyId>(
        item->data(0, Qt::UserRole + 2).toULongLong());
    if (bodyId != kInvalidBodyId) removeBody(bodyId);
  });
  connect(featureTree_, &QTreeWidget::itemChanged, this,
          [this](QTreeWidgetItem* item, int) {
            if (!partProjectActive()) return;
            const int kind = item->data(0, Qt::UserRole).toInt();
            const bool visible = item->checkState(0) == Qt::Checked;
            const auto bodyId = static_cast<BodyId>(
                item->data(0, Qt::UserRole + 2).toULongLong());
            if (kind == 4) {
              commitReferenceImageEdit();
              const auto imageId = static_cast<ReferenceImageId>(
                  item->data(0, Qt::UserRole + 3).toULongLong());
              auto* image = mutableStandaloneDocument().findReferenceImage(imageId);
              if (!image || image->visible == visible) return;
              const auto previousSelection = captureHistorySelection();
              Document previous = activePartDocument();
              image->visible = visible;
              if (!pushModelTransition(std::move(previous), previousSelection,
                                       captureCommittedEndState())) {
                const QSignalBlocker blocker(featureTree_);
                item->setCheckState(0,
                                    visible ? Qt::Unchecked : Qt::Checked);
                return;
              }
              viewport_->setReferenceImages(activePartDocument().referenceImages());
              if (imageId == selectedReferenceImageId_)
                showSelectedReferenceImageManipulator();
              completeModelTransitionUi();
              if (workspaceStack_->currentWidget() == sketchCanvas_)
                sketchCanvas_->updateSceneImages(
                    activePartDocument().referenceImages());
              return;
            }
            if (kind == 3 && bodyId != kInvalidBodyId) {
              Body* body = mutableStandaloneDocument().findBody(bodyId);
              if (!body || body->visible() == visible) return;
              const auto previousSelection = captureHistorySelection();
              Document previous = activePartDocument();
              auto committedAfter = captureCommittedEndState();
              body->setVisible(visible);
              if (!pushModelTransition(std::move(previous), previousSelection,
                                       std::move(committedAfter))) {
                const QSignalBlocker blocker(featureTree_);
                item->setCheckState(0, visible ? Qt::Unchecked : Qt::Checked);
                return;
              }
              resetTransientModelingUi();
              refreshBodyViewFromDocument();
              completeModelTransitionUi();
              return;
            }
            const HistorySelectionState before = captureHistorySelection();
            if (kind == 1) viewport_->setOriginVisible(visible);
            if (kind == 2) viewport_->setSketchVisible(visible);
            if (kind >= 10 && kind <= 12)
              viewport_->setBasePlaneVisible(kind - 10, visible);
            if (kind >= 20)
              viewport_->setSketchVisible(static_cast<std::size_t>(kind - 20), visible);
            const HistorySelectionState after = captureHistorySelection();
            if (!pushPresentationTransition(before, after)) {
              const QSignalBlocker blocker(featureTree_);
              item->setCheckState(0, visible ? Qt::Unchecked : Qt::Checked);
            }
          });
  connect(viewport_, &Viewport::revolveAxisPicked, this,
          [this](qulonglong axisToken) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle == ToolLifecycle::Inactive) return;
            invalidatePreviewUpdates();
            AxisReference axis;
            if (axisToken == Viewport::kGlobalXAxisToken) {
              axis.type = AxisReferenceType::GlobalX;
            } else if (axisToken == Viewport::kGlobalYAxisToken) {
              axis.type = AxisReferenceType::GlobalY;
            } else if (axisToken == Viewport::kGlobalZAxisToken) {
              axis.type = AxisReferenceType::GlobalZ;
            } else if (axisToken == 1) {
              axis.sketchId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileSketchId;
              axis.type = AxisReferenceType::SketchHorizontalAxis;
            } else if (axisToken == 2) {
              axis.sketchId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileSketchId;
              axis.type = AxisReferenceType::SketchVerticalAxis;
            } else {
              axis.sketchId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileSketchId;
              axis.type = AxisReferenceType::SketchLine;
              axis.lineId = static_cast<sketch::GeometryId>(axisToken - 3);
            }
            partDesignCoordinator_.setRevolveAxis(activePartDocument(), axis);
            partDesignCoordinator_.finishReselection();
            const int comboIndex = revolveAxisCombo_->findData(axisToken);
            if (comboIndex >= 0) {
              const QSignalBlocker blocker(revolveAxisCombo_);
              revolveAxisCombo_->setCurrentIndex(comboIndex);
            }
            updateRevolveToolPreview();
            static_cast<void>(viewport_->focusToolParameterField(false));
            statusBar()->showMessage(QString::fromUtf8("Задайте угол дугой или полем у манипулятора"));
          });
  connect(viewport_, &Viewport::moveBodyPicked, this, [this](BodyId bodyId) {
    if (!moveCommandContextActive()) return;
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive) return;
    invalidatePreviewUpdates();
    const Body* body = activePartDocument().findBody(bodyId);
    if (!body || !body->activeFeature() || !body->resultShape()) {
      statusBar()->showMessage(
          QString::fromUtf8("Выбранное тело не содержит геометрии"), 3000);
      return;
    }
    partDesignCoordinator_.setMoveBody(body->id(), body->activeFeature()->id(),
                             body->resultShape());
    partDesignCoordinator_.finishReselection();
    moveBodyValue_->setText(QString::fromStdString(body->name()));
    viewport_->showMovePreview();
    updateMoveToolPreview();
    static_cast<void>(viewport_->focusToolParameterField(false));
    statusBar()->showMessage(
        QString::fromUtf8("Потяните стрелку X, Y или Z либо введите смещение"));
  });
  connect(viewport_, &Viewport::translationToolManipulatorValueChanged, this,
          [this](int axisIndex, double value) {
            if (!moveCommandContextActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive)
              return;
            const QSignalBlocker xBlocker(moveXSpin_);
            const QSignalBlocker yBlocker(moveYSpin_);
            const QSignalBlocker zBlocker(moveZSpin_);
            if (axisIndex == 0)
              moveXSpin_->setValue(value);
            else if (axisIndex == 1)
              moveYSpin_->setValue(value);
            else if (axisIndex == 2)
              moveZSpin_->setValue(value);
            schedulePreviewUpdate(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId, partDesignCoordinator_.snapshot(PartDesignToolKind::Move).sourceFeatureId,
                [this, axisIndex, value] {
                  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle != ToolLifecycle::Inactive)
                    partDesignCoordinator_.setMoveOffsetComponent(axisIndex, value);
                },
                [this] { updateMoveToolPreview(); });
          });
  connect(viewport_, &Viewport::mirrorBodyPicked, this,
          [this](BodyId bodyId) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            Body* body = mutableStandaloneDocument().findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            partDesignCoordinator_.setMirrorBody(body->id(), body->activeFeature()->id(),
                                       body->resultShape(),
                                       body->activeFeature()->topologyIndex());
            partDesignCoordinator_.finishReselection();
            mirrorBodyValue_->setText(QString::fromStdString(body->name()));
            mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
            mirrorPlaneSelectButton_->setEnabled(true);
            viewport_->beginMirrorPlaneSelection();
            updateMirrorToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8(
                    "2/2 Выберите базовую плоскость или плоскую грань в 3D-виде"));
          });
  connect(viewport_, &Viewport::mirrorPlanePicked, this,
          [this](int planeIndex) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::Inactive ||
                planeIndex < 0 || planeIndex > 2)
              return;
            invalidatePreviewUpdates();
            const auto plane = static_cast<MirrorPlane>(planeIndex);
            partDesignCoordinator_.setMirrorPlane(plane);
            partDesignCoordinator_.finishReselection();
            mirrorPlaneValue_->setText(
                plane == MirrorPlane::XY
                    ? QStringLiteral("XY")
                    : plane == MirrorPlane::XZ ? QStringLiteral("XZ")
                                               : QStringLiteral("YZ"));
            updateMirrorToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("Предпросмотр зеркала построен"), 3000);
          });
  connect(viewport_, &Viewport::mirrorPlaneFacePicked, this,
          [this](const FaceReference& face) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror)
                    .lifecycle == ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            partDesignCoordinator_.setMirrorPlaneFace(face);
            partDesignCoordinator_.finishReselection();
            mirrorPlaneValue_->setText(
                face.isV1()
                    ? QString::fromUtf8("Грань #%1").arg(face.faceIndex() + 1)
                    : QString::fromUtf8("Грань (устойчивая ссылка)"));
            updateMirrorToolPreview();
            statusBar()->showMessage(
                partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror)
                        .lifecycle == ToolLifecycle::PreviewValid
                    ? QString::fromUtf8("Предпросмотр зеркала построен")
                    : QString::fromUtf8(
                          "Выбранная грань не может быть плоскостью зеркала"),
                3000);
          });
  connect(viewport_, &Viewport::linearPatternBodyPicked, this,
          [this](BodyId bodyId) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            Body* body = mutableStandaloneDocument().findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            partDesignCoordinator_.setLinearPatternBody(
                body->id(), body->activeFeature()->id(), body->resultShape());
            partDesignCoordinator_.finishReselection();
            linearPatternBodyValue_->setText(
                QString::fromStdString(body->name()));
            linearPatternAxisValue_->setText(
                QString::fromUtf8("Не выбрано"));
            linearPatternAxisSelectButton_->setEnabled(true);
            viewport_->beginLinearPatternAxisSelection();
            updateLinearPatternToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую ось в 3D-виде"));
          });
  connect(viewport_, &Viewport::linearPatternAxisPicked, this,
          [this](int axisIndex) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                    ToolLifecycle::Inactive ||
                axisIndex < 0 || axisIndex > 2)
              return;
            invalidatePreviewUpdates();
            const auto direction = static_cast<PrincipalAxis>(axisIndex);
            partDesignCoordinator_.setLinearPatternDirection(direction);
            partDesignCoordinator_.finishReselection();
            linearPatternAxisValue_->setText(
                axisIndex == 0 ? QStringLiteral("X")
                : axisIndex == 1 ? QStringLiteral("Y")
                                 : QStringLiteral("Z"));
            viewport_->showLinearPatternAxisSelection(axisIndex);
            updateLinearPatternToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("Предпросмотр линейного массива построен"),
                3000);
          });
  connect(viewport_, &Viewport::circularPatternBodyPicked, this,
          [this](BodyId bodyId) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            Body* body = mutableStandaloneDocument().findBody(bodyId);
            if (!body || !body->activeFeature() || !body->resultShape()) {
              statusBar()->showMessage(
                  QString::fromUtf8("Выбранное тело не содержит геометрии"),
                  3000);
              return;
            }
            partDesignCoordinator_.setCircularPatternBody(
                body->id(), body->activeFeature()->id(), body->resultShape());
            partDesignCoordinator_.finishReselection();
            circularPatternBodyValue_->setText(
                QString::fromStdString(body->name()));
            circularPatternAxisValue_->setText(
                QString::fromUtf8("Не выбрано"));
            circularPatternAxisSelectButton_->setEnabled(true);
            viewport_->beginCircularPatternAxisSelection();
            updateCircularPatternToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("2/2 Выберите базовую ось в 3D-виде"));
          });
  connect(viewport_, &Viewport::circularPatternAxisPicked, this,
          [this](int axisIndex) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                    ToolLifecycle::Inactive ||
                axisIndex < 0 || axisIndex > 2)
              return;
            invalidatePreviewUpdates();
            const auto axis = static_cast<PrincipalAxis>(axisIndex);
            partDesignCoordinator_.setCircularPatternAxis(axis);
            partDesignCoordinator_.finishReselection();
            circularPatternAxisValue_->setText(
                axisIndex == 0 ? QStringLiteral("X")
                : axisIndex == 1 ? QStringLiteral("Y")
                                 : QStringLiteral("Z"));
            viewport_->showCircularPatternAxisSelection(axisIndex);
            updateCircularPatternToolPreview();
            statusBar()->showMessage(
                QString::fromUtf8("Предпросмотр кругового массива построен"),
                3000);
          });
  connect(viewport_, &Viewport::draftAxisPicked, this,
          [this](int axisIndex) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).lifecycle == ToolLifecycle::Inactive ||
                axisIndex < 0 || axisIndex > 2)
              return;
            invalidatePreviewUpdates();
            if (!partDesignCoordinator_.setDraftPrincipalAxis(activePartDocument(), axisIndex)) return;
            partDesignCoordinator_.finishReselection();
            viewport_->showDraftAxisSelection(axisIndex);
            updateDraftToolPreview();
            static_cast<void>(viewport_->focusToolParameterField(false));
            statusBar()->showMessage(QString::fromUtf8(
                "3/3 Потяните дугу или введите угол и нажмите Enter"));
          });
  connect(viewport_, &Viewport::draftEdgeAxisPicked, this,
          [this](const EdgeReference& edge) {
            if (!partProjectActive()) return;
            if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).lifecycle == ToolLifecycle::Inactive)
              return;
            invalidatePreviewUpdates();
            if (!partDesignCoordinator_.setDraftRotationEdge(activePartDocument(), edge)) {
              updateDraftToolPreview();
              viewport_->beginDraftAxisSelection();
              statusBar()->showMessage(QString::fromUtf8(
                  "Ребро должно быть прямым и принадлежать выбранной поверхности"));
              return;
            }
            partDesignCoordinator_.finishReselection();
            viewport_->showDraftEdgeAxisSelection(edge);
            updateDraftToolPreview();
            static_cast<void>(viewport_->focusToolParameterField(false));
            statusBar()->showMessage(QString::fromUtf8(
                "3/3 Потяните дугу или введите угол и нажмите Enter"));
          });
  connect(viewport_, &Viewport::selectionChanged, this,
          [this](const QString& text) {
            statusBar()->showMessage(text.isEmpty()
                                         ? QString::fromUtf8("Выделение снято")
                                         : text);
          });
  connect(viewport_, &Viewport::interactionCancelled, this,
          [this](ViewportCancelReason reason) {
            if (!partProjectActive()) return;
            const auto effect = partDesignCoordinator_.dispatchActiveAction(
                PartDesignAction::Escape);
            if (effect.transition.effect != PartDesignTransitionEffect::None) {
              applyPartDesignUiEffect(effect);
              statusBar()->showMessage(
                  reason == ViewportCancelReason::NestedReselection
                      ? QString::fromUtf8("Повторный выбор отменён")
                      : QString::fromUtf8("Инструмент отменён"),
                  2000);
              return;
            }
            viewport_->clearLegacyExtrusionPreview();
            selectedExtrusionSource_.reset();
            if (extrusionDock_) extrusionDock_->hide();
            modelRibbon_->clearActiveTool();
            if (reason == ViewportCancelReason::SketchPlaneSelection)
              rebuildFeatureTree();
            if (reason == ViewportCancelReason::ImagePlaneSelection) {
              pendingReferenceImagePath_.clear();
              pendingReferenceImagePixelSize_ = {};
              rebuildFeatureTree();
            }
            statusBar()->showMessage(QString::fromUtf8("Инструменты сброшены"),
                                     2000);
          });
  modelTreeStack_ = new QStackedWidget(modelTreeDock_);
  partTreePage_ = new QWidget(modelTreeStack_);
  auto* partTreeLayout = new QVBoxLayout(partTreePage_);
  partTreeLayout->setContentsMargins(0, 0, 0, 0);
  partTreeLayout->setSpacing(0);
  partTreeLayout->addWidget(featureTree_);
  assemblyTreeView_ = new QTreeView(modelTreeStack_);
  assemblyTreeView_->setObjectName(QStringLiteral("assemblyTreeView"));
  assemblyTreeView_->setHeaderHidden(true);
  assemblyTreeView_->setAlternatingRowColors(true);
  assemblyTreeView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  assemblyTreeView_->setDragEnabled(false);
  assemblyTreeView_->setAcceptDrops(false);
  assemblyTreeView_->setContextMenuPolicy(Qt::CustomContextMenu);
  assemblyRemoveShortcut_ =
      new QShortcut(QKeySequence(Qt::Key_Delete), assemblyTreeView_);
  assemblyRemoveShortcut_->setObjectName(
      QStringLiteral("assemblyRemoveOccurrenceShortcut"));
  assemblyRemoveShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
  assemblyRemoveShortcut_->setEnabled(false);
  connect(assemblyRemoveShortcut_, &QShortcut::activated,
          assemblyRemoveAction_, &QAction::trigger);
  connect(assemblyTreeView_, &QTreeView::customContextMenuRequested, this,
          [this](const QPoint& position) {
            if (!assemblyTreeView_ || !assemblyTreeModel_ ||
                !assemblyRenameAction_ || !assemblyVisibilityAction_ ||
                !assemblyGroundingAction_ ||
                !assemblySuppressionAction_ ||
                !assemblyDuplicateAction_ ||
                !assemblyRemoveAction_ ||
                !assemblyEditDefinitionAction_ ||
                !assemblyIsolationAction_)
              return;
            const QModelIndex index = assemblyTreeView_->indexAt(position);
            const auto address = assemblyTreeModel_->addressForIndex(index);
            if (!address) return;
            assemblyTreeView_->setCurrentIndex(index);
            updateAssemblyRenameAvailability();
            QMenu menu(assemblyTreeView_);
            if (address->get().kind ==
                AssemblyTreeNodeKind::PartDefinition) {
              menu.addAction(assemblyEditDefinitionAction_);
              menu.exec(
                  assemblyTreeView_->viewport()->mapToGlobal(position));
              return;
            }
            if (address->get().kind != AssemblyTreeNodeKind::Occurrence ||
                !address->get().occurrence)
              return;
            menu.addAction(assemblyRenameAction_);
            menu.addAction(assemblyVisibilityAction_);
            menu.addAction(assemblyGroundingAction_);
            menu.addAction(assemblySuppressionAction_);
            menu.addAction(assemblyDuplicateAction_);
            menu.addAction(assemblyRemoveAction_);
            menu.addAction(assemblyIsolationAction_);
            menu.exec(assemblyTreeView_->viewport()->mapToGlobal(position));
          });
  connect(assemblyTreeView_, &QTreeView::doubleClicked, this,
          [this](const QModelIndex& index) {
            if (!assemblyTreeModel_ || !assemblyTreeView_) return;
            const auto address = assemblyTreeModel_->addressForIndex(index);
            if (!address || address->get().kind !=
                                AssemblyTreeNodeKind::PartDefinition)
              return;
            assemblyTreeView_->setCurrentIndex(index);
            enterAssemblyDefinitionEdit();
          });
  componentTreeView_ = new QTreeView(modelTreeStack_);
  componentTreeView_->setObjectName(QStringLiteral("componentTreeView"));
  componentTreeView_->setHeaderHidden(true);
  componentTreeView_->setAlternatingRowColors(true);
  componentTreeView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  componentTreeView_->setDragEnabled(false);
  componentTreeView_->setAcceptDrops(false);
  componentTreeView_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(componentTreeView_, &QTreeView::customContextMenuRequested, this,
          [this](const QPoint& position) {
            if (!componentTreeModel_ || !componentTreeView_) return;
            const QModelIndex index = componentTreeView_->indexAt(position);
            const auto address = componentTreeModel_->addressForIndex(index);
            if (!address) return;
            const ComponentTreeAddressValue value = address->get().value();
            componentTreeView_->setCurrentIndex(index);
            QMenu menu(componentTreeView_);
            QAction* activate = nullptr;
            const bool component =
                std::holds_alternative<ComponentTreeRootAddress>(value) ||
                std::holds_alternative<ComponentTreeOccurrenceAddress>(value);
            if (component)
              activate = menu.addAction(
                  QString::fromUtf8("Сделать компонент активным"));

            QAction* visibility = nullptr;
            if (componentTreeModel_->flags(index) & Qt::ItemIsUserCheckable) {
              const bool shown =
                  componentTreeModel_->data(index, Qt::CheckStateRole).toInt() !=
                  Qt::Unchecked;
              visibility = menu.addAction(
                  shown ? QString::fromUtf8("Скрыть")
                        : QString::fromUtf8("Показать"));
            }

            const bool editableEntity =
                std::holds_alternative<ComponentTreeOccurrenceAddress>(value) ||
                std::holds_alternative<ComponentTreeBodyAddress>(value) ||
                std::holds_alternative<ComponentTreeSketchAddress>(value) ||
                std::holds_alternative<ComponentTreeReferenceImageAddress>(
                    value);
            QAction* rename = nullptr;
            QAction* remove = nullptr;
            QAction* createComponent = nullptr;
            if (editableEntity) {
              menu.addSeparator();
              rename = menu.addAction(QString::fromUtf8("Переименовать"));
              if (const auto* body =
                      std::get_if<ComponentTreeBodyAddress>(&value)) {
                createComponent = menu.addAction(
                    QString::fromUtf8("Создать компонент из тела"));
                createComponent->setEnabled(
                    prepareComponentExtraction(
                        {{body->body.component.definitionId,
                          body->body.component.occurrencePath},
                         body->body.bodyId})
                        .has_value());
              }
              remove = menu.addAction(QString::fromUtf8("Удалить"));
            }

            QAction* chosen = menu.exec(
                componentTreeView_->viewport()->mapToGlobal(position));
            if (!chosen) return;
            if (chosen == activate)
              enterComponentDefinitionEdit(index);
            else if (chosen == visibility)
              static_cast<void>(componentTreeModel_->setData(
                  index,
                  componentTreeModel_->data(index, Qt::CheckStateRole)
                              .toInt() == Qt::Unchecked
                      ? Qt::Checked
                      : Qt::Unchecked,
                  Qt::CheckStateRole));
            else if (chosen == rename)
              static_cast<void>(componentTreeView_->edit(index));
            else if (chosen == createComponent) {
              const auto& body = std::get<ComponentTreeBodyAddress>(value);
              createComponentFromBody(
                  {{body.body.component.definitionId,
                    body.body.component.occurrencePath},
                   body.body.bodyId});
            }
            else if (chosen == remove)
              deleteComponentTreeItem(value);
          });
  connect(componentTreeView_, &QTreeView::doubleClicked, this,
          [this](const QModelIndex& index) {
            if (!componentTreeModel_) return;
            const auto address = componentTreeModel_->addressForIndex(index);
            if (!address) return;
            if (std::holds_alternative<ComponentTreeDrawingAddress>(
                    address->get().value())) {
              activateComponentDrawing(index);
              return;
            }
            if (std::holds_alternative<ComponentTreeRootAddress>(
                    address->get().value()) ||
                std::holds_alternative<ComponentTreeOccurrenceAddress>(
                    address->get().value()))
              enterComponentDefinitionEdit(index);
          });
  modelTreeStack_->addWidget(partTreePage_);
  modelTreeStack_->addWidget(assemblyTreeView_);
  modelTreeStack_->addWidget(componentTreeView_);
  modelTreeStack_->setCurrentWidget(partTreePage_);
  modelTreeDock_->setWidget(modelTreeStack_);
  addDockWidget(Qt::LeftDockWidgetArea, modelTreeDock_);

  connect(featureTree_, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem* current) {
            if (!partProjectActive()) return;
            commitReferenceImageEdit();
            if (!current || current->data(0, Qt::UserRole).toInt() != 4) {
              selectedReferenceImageId_ = kInvalidReferenceImageId;
              if (viewport_->referenceImageManipulatorActive())
                viewport_->clearToolManipulator();
              return;
            }
            resetTransientModelingUi();
            modelRibbon_->clearActiveTool();
            selectedReferenceImageId_ = static_cast<ReferenceImageId>(
                current->data(0, Qt::UserRole + 3).toULongLong());
            showSelectedReferenceImageManipulator();
          });
  connect(featureTree_, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem*, QTreeWidgetItem*) {
            updateCreateComponentFromBodyAvailability();
          });

  connect(viewport_, &Viewport::assemblyOccurrenceSelected, this,
          &MainWindow::selectAssemblyOccurrenceFromViewport,
          Qt::DirectConnection);
  connect(viewport_, &Viewport::assemblyOccurrenceSelectionCleared, this,
          [this](const AssemblyScenePublicationHandle& publication) {
            if (projectKind_ != EditorProjectKind::AssemblyReadOnly ||
                assemblySelectionReentry_ || !assemblyTreeView_ ||
                !assemblyTreeView_->selectionModel() || !viewport_ ||
                !viewport_->assemblySession() ||
                !publication.isSameCurrentPublication(
                    viewport_->assemblySession()->publicationHandle()))
              return;
            assemblySelectionReentry_ = true;
            const QSignalBlocker blocker(assemblyTreeView_->selectionModel());
            assemblyTreeView_->clearSelection();
            assemblyTreeView_->setCurrentIndex({});
            assemblySelectionReentry_ = false;
            updateAssemblyRenameAvailability();
          },
          Qt::DirectConnection);
  connect(viewport_, &Viewport::componentSelected, this,
          &MainWindow::selectComponentFromViewport, Qt::DirectConnection);
  connect(viewport_, &Viewport::componentPlacementDragRequested, this,
          &MainWindow::commitComponentPlacementDrag, Qt::DirectConnection);
  connect(viewport_, &Viewport::componentSelectionCleared, this,
          [this](const ComponentScenePublicationHandle& publication) {
            QPointer<MainWindow> receiver(this);
            if ((!componentProductActive() &&
                 !rootComponentAuthoringActive()) ||
                componentSelectionReentry_ ||
                !componentTreeView_ ||
                !componentTreeView_->selectionModel() || !viewport_ ||
                !viewport_->componentSession() ||
                !publication.isSameCurrentPublication(
                    viewport_->componentSession()->publicationHandle()))
              return;
            componentSelectionReentry_ = true;
            const QSignalBlocker blocker(
                componentTreeView_->selectionModel());
            componentTreeView_->clearSelection();
            componentTreeView_->setCurrentIndex({});
            if (!receiver) return;
            componentSelectionReentry_ = false;
            if (!viewport_ || !viewport_->componentSession() ||
                !publication.isSameCurrentPublication(
                    viewport_->componentSession()->publicationHandle()))
              return;
            updateCreateComponentFromBodyAvailability();
          },
          Qt::DirectConnection);

  historyDock_ =
      new QDockWidget(QString::fromUtf8("История построений"), this);
  historyDock_->setObjectName(QStringLiteral("historyDock"));
  historyDock_->setAllowedAreas(Qt::BottomDockWidgetArea);
  historyDock_->setFeatures(QDockWidget::DockWidgetClosable);
  historyDock_->setMinimumHeight(96);
  historyDock_->setMaximumHeight(108);
  auto* historyHost = new QWidget(historyDock_);
  auto* historyHostLayout = new QVBoxLayout(historyHost);
  historyHostLayout->setContentsMargins(8, 2, 8, 3);
  historyHostLayout->setSpacing(1);
  historyScroll_ = new QScrollArea(historyDock_);
  historyScroll_->setWidgetResizable(true);
  historyScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  historyScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  historyScroll_->setFrameShape(QFrame::NoFrame);
  historyTimeline_ = new HistoryTimelineWidget(historyScroll_);
  historyContent_ = historyTimeline_;
  historyLayout_ = historyTimeline_->stepLayout();
  historyScroll_->setWidget(historyContent_);
  connect(historyTimeline_, &HistoryTimelineWidget::positionChanged, this,
          [this](int position) {
            if (!partProjectActive()) return;
            resetTransientModelingUi();
            applyHistoryPosition(position);
          });
  historyHostLayout->addWidget(historyScroll_, 1);
  historyDock_->setWidget(historyHost);
  addDockWidget(Qt::BottomDockWidgetArea, historyDock_);
  rebuildHistoryPanel();

  statusBar()->showMessage(
      QString::fromUtf8("Готово к параметрическому моделированию"));
}

void MainWindow::updateFromSketch(double widthMm, double heightMm) {
  if (!partProjectActive()) return;
  invalidatePreviewUpdates();
  setWindowModified(true);
  if (widthMm <= 0.0 || heightMm <= 0.0) {
    statusBar()->showMessage(QString::fromUtf8("Эскиз пуст"));
    return;
  }
  // Editing a sketch must not resize an existing solid. Body dimensions are
  // changed only when a modelling operation (for example extrusion) commits.
  viewport_->setSketch(sketchCanvas_->sketch(), currentSketchPlacement_);
  drawingSheet_->setRectangle(widthMm, heightMm);
  statusBar()->showMessage(
      QString::fromUtf8("Геометрия эскиза обновлена"), 1200);
}

void MainWindow::finishSketch() {
  if (!partProjectActive()) return;
  if (!editingSketchIndex_ && currentSketchFaceReference_ &&
      currentSketchFaceReference_->isV2() &&
      projectPersistenceState_.format ==
          project::ProjectPersistenceFormat::V3) {
    statusBar()->showMessage(
        QString::fromUtf8(
            "Для эскиза на грани сначала выберите «Файл → Обновить формат проекта до v4…»."),
        6000);
    return;
  }
  invalidatePreviewUpdates();
  const auto& sketch = sketchCanvas_->sketch();
  // Sketcher owns its orbit while it is active.  Transfer that global camera
  // back before exposing the 3D widget; otherwise the first extrusion preview
  // reuses the stale pre-Sketcher hemisphere and the ViewCube can say Bottom
  // while the user has just been looking at Top (or vice versa).
  viewport_->adoptSketchView(sketchCanvas_->viewCubeCamera(),
                             {}, sketchCanvas_->worldOriginScreenPosition(),
                             sketchCanvas_->viewPixelsPerMillimeter());
  viewport_->preserveWorldOriginOnNextResize();
  const HistorySelectionState previousSelection = captureHistorySelection();
  QString completionMessage =
      QString::fromUtf8("Эскиз завершён — модель перестроена");
  if (sketch.lines().empty() && sketch.circles().empty() &&
      sketch.arcs().empty() && sketch.beziers().empty()) {
    workspaceStack_->setCurrentWidget(viewport_);
    statusBar()->showMessage(
        QString::fromUtf8("Пустой эскиз закрыт без сохранения"), 3000);
    return;
  }
  if (editingSketchIndex_) {
    const std::size_t index = *editingSketchIndex_;
    if (index >= sketchViews_.size() ||
        !activePartDocument().findSketch(sketchViews_[index].documentSketchId)) {
      editingSketchIndex_.reset();
      workspaceStack_->setCurrentWidget(viewport_);
      statusBar()->showMessage(
          QString::fromUtf8("Эскиз больше не существует в документе"), 4000);
      return;
    }
  }
  updateFromSketch(sketch.widthMm(), sketch.heightMm());
  if (editingSketchIndex_) {
    const std::size_t index = *editingSketchIndex_;
    const SketchId sketchId = sketchViews_[index].documentSketchId;
    Document previousDocument = activePartDocument();
    if (!mutableStandaloneDocument().replaceSketchGeometry(sketchId, sketch)) {
      editingSketchIndex_.reset();
      workspaceStack_->setCurrentWidget(viewport_);
      statusBar()->showMessage(
          QString::fromUtf8("Эскиз больше не существует в документе"), 4000);
      return;
    }
    mutableStandaloneDocument().findSketch(sketchId)->placement = currentSketchPlacement_;
    const bool recomputeSucceeded = mutableStandaloneDocument().recompute();
    if (!recomputeSucceeded) {
      completionMessage =
          QString::fromUtf8(
              "Эскиз обновлён, перестроение модели завершилось с ошибкой: %1")
              .arg(QString::fromStdString(activePartDocument().rebuildError()));
    }
    auto committedAfter = captureCommittedEndState();
    if (!pushModelTransition(std::move(previousDocument), previousSelection,
                             std::move(committedAfter)))
      return;
    refreshBodyViewFromDocument();
    const auto* committedSketch = activePartDocument().findSketch(sketchId);
    viewport_->updateSketch(index, sketch,
                            sketchPresentationLabel(*committedSketch),
                            currentSketchPlacement_);
    if (hasHistoricalLegacyExtrusion() &&
        historicalLegacyExtrusionSourceSketchId_ == sketchId)
      viewport_->setSolidSketch(sketch, currentSketchPlacement_);
  } else {
    Document previousDocument = activePartDocument();
    auto& modelSketch = mutableStandaloneDocument().addSketch(
        "Sketch " + std::to_string(activePartDocument().sketches().size() + 1));
    modelSketch.geometry = sketch;
    modelSketch.placement = currentSketchPlacement_;
    if (currentSketchFaceReference_)
      mutableStandaloneDocument().attachSketchToFace(modelSketch.id,
                                   *currentSketchFaceReference_);
    const SketchId modelSketchId = modelSketch.id;
    auto committedAfter = captureCommittedEndState();
    committedAfter.sketchViewIds.push_back(modelSketchId);
    committedAfter.presentation.sketchVisibilities.push_back(1U);
    if (!pushModelTransition(std::move(previousDocument), previousSelection,
                             std::move(committedAfter)))
      return;
    viewport_->addSketch(modelSketchId, sketch,
                         sketchPresentationLabel(modelSketch),
                         currentSketchPlacement_);
    sketchViews_.push_back({modelSketchId});
  }
  editingSketchIndex_.reset();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  applyHistoryPosition(historyPosition_);
  workspaceStack_->setCurrentWidget(viewport_);
  completeModelTransitionUi();
  statusBar()->showMessage(completionMessage, 5000);
}

void MainWindow::normalizeExtrusionDistance() {
  if (!partProjectActive()) return;
  const auto input = normalizeExtrusionInput(
      extrusionLengthSpin_->value(), extrusionReverseCheck_->isChecked());
  const QSignalBlocker distanceBlocker(extrusionLengthSpin_);
  const QSignalBlocker reverseBlocker(extrusionReverseCheck_);
  extrusionLengthSpin_->setValue(input.distanceMm);
  extrusionReverseCheck_->setChecked(input.reversed);
  viewport_->setExtrusionPreviewLength(input.reversed ? -input.distanceMm
                                                       : input.distanceMm);
  scheduleAutomaticExtrudeOperation();
}

void MainWindow::updateLegacyExtrusionPreview() {
  if (!partProjectActive()) return;
  if (!selectedExtrusionSource_ ||
      !viewport_->extrusionManipulatorVisible()) {
    viewport_->clearToolPreviewShape();
    return;
  }

  DocumentSketch profile;
  if (const auto* region =
          std::get_if<SketchRegionPick>(&selectedExtrusionSource_->source)) {
    if (const auto* source = activePartDocument().findSketch(region->sketchId))
      profile = *source;
    profile.id = region->sketchId;
    profile.geometry = region->geometry;
    profile.placement = region->placement;
  } else if (const auto* face = std::get_if<LegacySolidFacePick>(
                 &selectedExtrusionSource_->source)) {
    profile.id = 1;
    profile.geometry = face->geometry;
    profile.placement = face->placement;
  } else {
    viewport_->clearToolPreviewShape();
    return;
  }

  const auto input = normalizeExtrusionInput(
      extrusionLengthSpin_->value(), extrusionReverseCheck_->isChecked());
  const auto operation = static_cast<ExtrudeOperation>(
      std::clamp(extrusionOperationCombo_->currentIndex(), 0, 2));
  const Body* body = activePartDocument().activeBody();
  const auto baseShape = body ? body->resultShape() : ShapeFeature::ShapePtr{};
  if (operation != ExtrudeOperation::NewBody && !baseShape) {
    viewport_->clearToolPreviewShape();
    return;
  }

  TopoDS_Shape preview;
  SketchExtrudeGeometry geometry;
  std::string error;
  OperationFailureCode failure{OperationFailureCode::None};
  if (!buildExtrusionFromSketch(
          profile,
          operation == ExtrudeOperation::NewBody ? nullptr : baseShape.get(),
          input.distanceMm, operation, input.reversed, &preview, &geometry,
          &error, &failure) ||
      preview.IsNull()) {
    viewport_->clearToolPreviewShape();
    return;
  }

  const BodyId bodyId = body ? body->id() : kInvalidBodyId;
  const FeatureId featureId = body && body->activeFeature()
                                  ? body->activeFeature()->id()
                                  : kInvalidFeatureId;
  viewport_->setToolPreviewPresentation(
      operation == ExtrudeOperation::NewBody
          ? ToolPreviewPresentation::OverlaySourceSelection
          : ToolPreviewPresentation::ReplaceSource);
  viewport_->setToolPreviewShape(
      bodyId, featureId,
      std::make_shared<const TopoDS_Shape>(std::move(preview)));
  viewport_->setToolPreviewReplacedBodies(
      operation == ExtrudeOperation::NewBody || bodyId == kInvalidBodyId
          ? std::vector<BodyId>{}
          : std::vector<BodyId>{bodyId});
}

std::optional<ExtrudeOperation>
MainWindow::detectAutomaticExtrudeOperation() const {
  if (extrudeOperationManuallyChanged_) return std::nullopt;
  ExtrudeOperation operation = ExtrudeOperation::NewBody;
  const Body* body = activePartDocument().activeBody();
  const std::size_t index = viewport_->extrusionCandidateSketchIndex();
  const DocumentSketch* profile =
      index < sketchViews_.size()
          ? activePartDocument().findSketch(sketchViews_[index].documentSketchId)
          : nullptr;
  const auto targetShape = body ? body->resultShape() : ShapeFeature::ShapePtr{};
  if (profile && targetShape) {
    DocumentSketch operationProfile = *profile;
    const auto& pickedProfile = viewport_->extrusionCandidateSketch();
    if (!pickedProfile.lines().empty() || !pickedProfile.circles().empty() ||
        !pickedProfile.arcs().empty() || !pickedProfile.beziers().empty())
      operationProfile.geometry = pickedProfile;
    operation = detectExtrudeOperation(
        operationProfile, extrusionLengthSpin_->value(),
        extrusionReverseCheck_->isChecked(), targetShape.get(),
        profile->support.type == SketchSupportType::Face);
  }
  return operation;
}

void MainWindow::updateAutomaticExtrudeOperation() {
  if (!partProjectActive()) return;
  const auto operation = detectAutomaticExtrudeOperation();
  if (!operation) return;
  const QSignalBlocker blocker(extrusionOperationCombo_);
  extrusionOperationCombo_->setCurrentIndex(static_cast<int>(*operation));
  updateLegacyExtrusionPreview();
}

void MainWindow::scheduleAutomaticExtrudeOperation() {
  if (!partProjectActive()) return;
  if (extrudeOperationManuallyChanged_) return;
  const Body* body = mutableStandaloneDocument().activeBody();
  const BodyId bodyId = body ? body->id() : kInvalidBodyId;
  const FeatureId featureId = body && body->activeFeature()
                                  ? body->activeFeature()->id()
                                  : kInvalidFeatureId;
  const ShapeRevision shapeRevision =
      body && body->activeFeature() ? body->activeFeature()->shapeRevision()
                                    : kInvalidShapeRevision;
  AutomaticExtrudeDetectionKey key{
      previewDocumentGeneration_, bodyId, featureId, shapeRevision,
      viewport_->extrusionCandidateSketchIndex(),
      viewport_->extrusionCandidateSupport(),
      viewport_->extrusionPreviewBaseBounds(),
      extrusionLengthSpin_->value(), extrusionReverseCheck_->isChecked()};
  if (automaticExtrudeDetectionKey_ == key) return;
  automaticExtrudeDetectionKey_ = std::move(key);
  auto detected = std::make_shared<std::optional<ExtrudeOperation>>();
  schedulePreviewUpdate(
      bodyId, featureId,
      [this, detected] {
        ++automaticExtrudeDetectionCount_;
        *detected = detectAutomaticExtrudeOperation();
      },
      [this, detected] {
        if (!*detected) return;
        const QSignalBlocker blocker(extrusionOperationCombo_);
        extrusionOperationCombo_->setCurrentIndex(
            static_cast<int>(**detected));
        updateLegacyExtrusionPreview();
      });
}

void MainWindow::extrudeSketch() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  invalidatePreviewUpdates();
  if (!ensureHistoryAtEnd()) return;
  const auto* legacySource =
      selectedExtrusionSource_
          ? std::get_if<LegacySolidFacePick>(
                &selectedExtrusionSource_->source)
          : nullptr;
  const auto& pickedSketch = viewport_->extrusionCandidateSketch();
  const std::size_t pickedIndex = viewport_->extrusionCandidateSketchIndex();
  const std::size_t sourceIndex =
      pickedIndex != static_cast<std::size_t>(-1)
          ? pickedIndex
          : !sketchViews_.empty() ? sketchViews_.size() - 1
                             : static_cast<std::size_t>(-1);
  DocumentSketch* modelSketch =
      sourceIndex < sketchViews_.size()
          ? mutableStandaloneDocument().findSketch(sketchViews_[sourceIndex].documentSketchId)
          : nullptr;
  // The viewport owns the user's exact region pick.  The source DocumentSketch
  // may contain several independent contours, so preferring the whole sketch
  // here loses the selection and makes Apply fail with "one profile at a time".
  const bool hasPickedProfile =
      !pickedSketch.lines().empty() || !pickedSketch.circles().empty() ||
      !pickedSketch.arcs().empty() || !pickedSketch.beziers().empty();
  const sketch::Sketch sketch =
      hasPickedProfile
          ? pickedSketch
          : modelSketch ? modelSketch->geometry : sketchCanvas_->sketch();
  if (sketch.lines().empty() && sketch.circles().empty() &&
      sketch.arcs().empty() && sketch.beziers().empty()) {
    QMessageBox::information(this, QString::fromUtf8("Выдавливание"),
                             QString::fromUtf8("Сначала создайте замкнутый контур эскиза."));
    return;
  }
  // Use the same authoritative multi-region validation as preview and final
  // recompute. Sketch::isClosed() is a single-sketch convenience check and,
  // in particular, rejects a valid selection containing both circular and
  // line/Arc regions.
  DocumentSketch selectedProfile = modelSketch ? *modelSketch : DocumentSketch{};
  selectedProfile.geometry = sketch;
  selectedProfile.placement = legacySource
                                  ? legacySource->placement
                                  : modelSketch ? modelSketch->placement
                                                : currentSketchPlacement_;
  if (selectedProfile.id == kInvalidSketchId) selectedProfile.id = 1;
  std::string profileError;
  OperationFailureCode profileFailureCode{OperationFailureCode::None};
  if (!isSupportedSketchProfile(selectedProfile, &profileError,
                                &profileFailureCode)) {
    const bool openContour =
        profileFailureCode == OperationFailureCode::InvalidProfileOpen;
    QMessageBox::warning(
        this,
        openContour ? QString::fromUtf8("Контур не замкнут")
                    : QString::fromUtf8("Некорректный профиль"),
        openContour
            ? QString::fromUtf8(
                  "Соедините конечные точки линий замкнутого контура.")
            : profileFailureCode == OperationFailureCode::InvalidProfileOverlap
                  ? QString::fromUtf8(
                        "Выбранные области пересекаются или образуют отверстие.")
                  : QString::fromUtf8("Выбранный профиль нельзя выдавить."));
    return;
  }

  const auto input = normalizeExtrusionInput(
      extrusionLengthSpin_->value(), extrusionReverseCheck_->isChecked());
  const double height = input.distanceMm;
  if (!std::isfinite(height) || height == 0.0) {
    QMessageBox::warning(this, QString::fromUtf8("Некорректная длина"),
                         QString::fromUtf8("Длина выдавливания не должна быть равна нулю."));
    return;
  }
  const auto operation = static_cast<ExtrudeOperation>(
      std::clamp(extrusionOperationCombo_->currentIndex(), 0, 2));
  const bool reversed = input.reversed;
  if (operation != ExtrudeOperation::NewBody && !activePartDocument().activeBody()) {
    QMessageBox::warning(this, QString::fromUtf8("Выдавливание"),
                         QString::fromUtf8(
                             "Для объединения или вырезания нужен активный Body."));
    return;
  }

  Document previousDocument = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (!modelSketch || legacySource) {
    modelSketch = &mutableStandaloneDocument().addSketch(
        "Extrude profile " + std::to_string(activePartDocument().sketches().size() + 1));
    modelSketch->geometry = sketch;
    modelSketch->placement = selectedProfile.placement;
  }
  Body* modelBody = operation == ExtrudeOperation::NewBody
                        ? &mutableStandaloneDocument().addBody()
                        : mutableStandaloneDocument().activeBody();
  auto extrudeFeature = std::make_unique<ExtrudeFeature>(
      modelSketch->id, height,
      "Extrude " + std::to_string(modelBody->features().size() + 1),
      operation, reversed);
  // If this feature references an existing sketch, persist the exact selected
  // region inside the feature.  Do not create a hidden duplicate DocumentSketch.
  if (hasPickedProfile)
    extrudeFeature->setProfileOverride(sketch);
  modelBody->addFeature(std::move(extrudeFeature));
  if (!mutableStandaloneDocument().rebuild()) {
    const std::string error = activePartDocument().rebuildError();
    mutableStandaloneDocument() = previousDocument;
    refreshBodyViewFromDocument();
    QMessageBox::warning(this, QString::fromUtf8("Ошибка выдавливания"),
                         QString::fromStdString(error));
    return;
  }
  // The v1 prism is only a compatibility presentation. Once the user commits
  // a real parametric Body it must never reappear after hiding or deleting the
  // modern result. Undo restores the stable source ID through
  // HistorySelectionState.
  auto committedAfter = captureCommittedEndState();
  committedAfter.historicalLegacyExtrusionSourceSketchId.reset();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  historicalLegacyExtrusionSourceSketchId_.reset();
  viewport_->clearToolPreviewShape();
  refreshBodyViewFromDocument();
  viewport_->setSketch(sketch);
  selectedExtrusionSource_.reset();
  viewport_->hideExtrusionManipulator();
  extrusionDock_->hide();
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  moveHistoryToEnd();
  applyHistoryPosition(historyPosition_);
  workspaceStack_->setCurrentWidget(viewport_);
  completeModelTransitionUi();
  statusBar()->showMessage(
      QString::fromUtf8("Создано твёрдое тело: выдавливание %1 мм").arg(height), 4000);
}

void MainWindow::syncSketchPresentationFromDocument() {
  if (historicalLegacyExtrusionSourceSketchId_ &&
      !activePartDocument().findSketch(*historicalLegacyExtrusionSourceSketchId_))
    historicalLegacyExtrusionSourceSketchId_.reset();
  for (std::size_t index = sketchViews_.size(); index-- > 0;) {
    if (activePartDocument().findSketch(sketchViews_[index].documentSketchId)) continue;
    sketchViews_.erase(sketchViews_.begin() +
                       static_cast<std::ptrdiff_t>(index));
    if (editingSketchIndex_) {
      if (*editingSketchIndex_ == index)
        editingSketchIndex_.reset();
      else if (*editingSketchIndex_ > index)
        --*editingSketchIndex_;
    }
  }

  std::vector<SketchPresentationSnapshot> snapshots;
  snapshots.reserve(sketchViews_.size());
  for (const auto& view : sketchViews_) {
    const auto* modelSketch = activePartDocument().findSketch(view.documentSketchId);
    if (!modelSketch) continue;
    snapshots.push_back({modelSketch->id, modelSketch->geometry,
                         sketchPresentationLabel(*modelSketch),
                         modelSketch->placement});
  }
  viewport_->replaceSketchPresentations(std::move(snapshots));
}

bool MainWindow::hasCommittedModernSolid() const noexcept {
  return std::any_of(activePartDocument().bodies().begin(), activePartDocument().bodies().end(),
                     [](const Body& body) {
                       return !body.features().empty();
                     });
}

std::optional<MainWindow::ModernSolidPresentation>
MainWindow::resolveModernSolidPresentation(
    const Body& body, const ShapeFeature::ShapePtr& candidate,
    bool effectivelyVisible) {
  if (!effectivelyVisible || !candidate || candidate->IsNull())
    return std::nullopt;
  const ShapeFeature* owner = presentationOwner(body, candidate);
  if (!owner) return std::nullopt;
  std::string topologyError;
  auto topology = owner->lastValidTopologyIndex(&topologyError);
  if (!topology) return std::nullopt;
  return ModernSolidPresentation{owner, candidate, std::move(topology)};
}

bool MainWindow::hasDisplayableModernSolid() const {
  return std::any_of(
      activePartDocument().bodies().begin(), activePartDocument().bodies().end(),
      [](const Body& body) {
        return resolveModernSolidPresentation(
                   body, body.lastValidResultShape(), body.visible())
            .has_value();
      });
}

bool MainWindow::hasExportableModernSolid(QString* error) const {
  const auto result = projectApplicationService_.preflightStlExport(activePartDocument());
  if (!result.succeeded() && error) *error = result.detail;
  return result.succeeded();
}

bool MainWindow::hasHistoricalLegacyExtrusion() const noexcept {
  return historicalLegacyExtrusionSourceSketchId_ &&
         activePartDocument().findSketch(*historicalLegacyExtrusionSourceSketchId_);
}

void MainWindow::refreshBodyViewFromDocument(
    const std::vector<BodyId>& transientVisibleBodies) {
  if (!partModelingContextActive()) return;
  viewport_->setReferenceImages(activePartDocument().referenceImages());
  if (historicalLegacyExtrusionSourceSketchId_ &&
      !activePartDocument().findSketch(*historicalLegacyExtrusionSourceSketchId_))
    historicalLegacyExtrusionSourceSketchId_.reset();
  // The historical marker is part of committed Undo/Redo state and directly
  // gates product conversion. Keep the action synchronized whenever model
  // refresh consumes, restores or clears that compatibility state.
  updateUpgradeProjectFormatAction();
  std::vector<BodyViewShape> shapes;
  for (const Body& body : activePartDocument().bodies()) {
    // Rendering explicitly uses the last validated committed result. The
    // authoritative resultShape() stays empty while the active Feature is in
    // Error, so tool/dependency code cannot consume this presentation fallback.
    const bool effectivelyVisible =
        body.visible() ||
        std::find(transientVisibleBodies.begin(), transientVisibleBodies.end(),
                  body.id()) != transientVisibleBodies.end();
    auto presentation = resolveModernSolidPresentation(
        body, body.lastValidResultShape(), effectivelyVisible);
    if (!presentation) continue;
    shapes.push_back(
        {body.id(), presentation->owner->id(), std::move(presentation->shape),
         std::move(presentation->topologyIndex),
         presentation->owner->shapeRevision()});
  }
  bool showHistoricalLegacyExtrusion = false;
  const bool anyVisibleBodyShape = !shapes.empty();
  viewport_->setBodyShapes(std::move(shapes));
  if (activePartDocument().bodies().empty() && hasHistoricalLegacyExtrusion()) {
    const auto* sourceSketch = activePartDocument().findSketch(
        *historicalLegacyExtrusionSourceSketchId_);
    // resetScene() deliberately clears the cached legacy mesh. Reconstruct it
    // from the authoritative Sketch selected by stable ID on every refresh.
    viewport_->setSolidSketch(sourceSketch->geometry, sourceSketch->placement);
    viewport_->setSolidSupport(sketchPresentationLabel(*sourceSketch));
    showHistoricalLegacyExtrusion = true;
  }
  // Keep document occupancy and viewport visibility separate.  A hidden last
  // Body still means the document contains Part Design geometry, but the
  // viewport must not enable its legacy box fallback after setBodyShapes({})
  // has cleared the B-Rep mesh.
  viewport_->setSolidVisible(anyVisibleBodyShape ||
                             showHistoricalLegacyExtrusion);
  drawingSheet_->setDocument(activePartDocument());
  // A view entry can only observe an existing committed sketch. If an
  // interrupted/corrupt transition leaves a stale ID, discard the derived
  // presentation and repair its positional UI references; never keep drawing
  // the old cached geometry or map the entry to a different sketch.
  for (std::size_t index = sketchViews_.size(); index-- > 0;) {
    if (activePartDocument().findSketch(sketchViews_[index].documentSketchId)) continue;
    viewport_->removeSketch(index);
    sketchViews_.erase(sketchViews_.begin() +
                       static_cast<std::ptrdiff_t>(index));
    if (editingSketchIndex_) {
      if (*editingSketchIndex_ == index)
        editingSketchIndex_.reset();
      else if (*editingSketchIndex_ > index)
        --*editingSketchIndex_;
    }
  }
  for (std::size_t index = 0; index < sketchViews_.size(); ++index) {
    const auto* modelSketch =
        activePartDocument().findSketch(sketchViews_[index].documentSketchId);
    if (!modelSketch) continue;
    viewport_->updateSketch(index, modelSketch->geometry,
                            sketchPresentationLabel(*modelSketch),
                            modelSketch->placement);
  }
}

void MainWindow::createRevolve() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  Body* body = mutableStandaloneDocument().activeBody();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginRevolve(
              activePartDocument(), body ? body->id() : kInvalidBodyId,
              body && body->activeFeature() ? body->activeFeature()->id()
                                            : kInvalidFeatureId,
              body ? body->resultShape() : ShapeFeature::ShapePtr{}),
          PartDesignToolKind::Revolve))
    return;
  {
    const QSignalBlocker blocker(revolveProfileCombo_);
    revolveProfileCombo_->clear();
    revolveProfileCombo_->addItem(QString::fromUtf8("Выбрать профиль"),
                                  QVariant::fromValue<qulonglong>(kInvalidSketchId));
    for (const auto& sketch : activePartDocument().sketches())
      revolveProfileCombo_->addItem(QString::fromStdString(sketch.name),
                                    QVariant::fromValue<qulonglong>(sketch.id));
  }
  rebuildRevolveAxisChoices();
  revolveAngleSpin_->setValue(360.0);
  const auto initialOperation = body && body->resultShape()
                                    ? ExtrudeOperation::Join
                                    : ExtrudeOperation::NewBody;
  {
    const QSignalBlocker blocker(revolveOperationCombo_);
    revolveOperationCombo_->setCurrentIndex(
        static_cast<int>(initialOperation));
  }
  partDesignCoordinator_.setRevolveOperation(activePartDocument(), initialOperation);
  revolveReverseCheck_->setChecked(false);
  revolveProfileSummary_->setText(QString::fromUtf8(
      "Щёлкните область в 3D-виде. Ctrl добавляет или убирает области."));
  revolveDock_->show(); revolveDock_->raise();
  viewport_->beginExtrusionSurfaceSelection();
  updateRevolveToolPreview();
  statusBar()->showMessage(
      QString::fromUtf8("Выберите профиль мышью во viewport"));
}

void MainWindow::updateRevolveProfileSelection(
    const ExtrusionSourcePick& pick) {
  if (!partProjectActive()) return;
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle == ToolLifecycle::Inactive) return;
  invalidatePreviewUpdates();

  const auto* region = std::get_if<SketchRegionPick>(&pick.source);
  const sketch::Sketch emptyProfile;
  const auto& selectedProfile = region ? region->geometry : emptyProfile;
  const bool empty = selectedProfile.lines().empty() &&
                     selectedProfile.circles().empty() &&
                     selectedProfile.arcs().empty() &&
                     selectedProfile.beziers().empty();
  const SketchId id = region ? region->sketchId : kInvalidSketchId;
  if (empty || id == kInvalidSketchId || !activePartDocument().findSketch(id)) {
    partDesignCoordinator_.clearRevolveAxis(activePartDocument());
    partDesignCoordinator_.clearRevolveProfile(activePartDocument());
    {
      const QSignalBlocker blocker(revolveProfileCombo_);
      revolveProfileCombo_->setCurrentIndex(0);
    }
    revolveProfileSummary_->setText(QString::fromUtf8(
        "Профиль не выбран. Щёлкните область в 3D-виде."));
    viewport_->beginExtrusionSurfaceSelection();
    updateRevolveToolPreview();
    statusBar()->showMessage(QString::fromUtf8("Выберите профиль вращения"));
    return;
  }

  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileSketchId != id &&
      partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).axisReference &&
      partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).axisReference->type != AxisReferenceType::GlobalX &&
      partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).axisReference->type != AxisReferenceType::GlobalY &&
      partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).axisReference->type != AxisReferenceType::GlobalZ)
    partDesignCoordinator_.clearRevolveAxis(activePartDocument());
  partDesignCoordinator_.setRevolveProfile(activePartDocument(), id, selectedProfile);
  partDesignCoordinator_.finishReselection();
  {
    const QSignalBlocker blocker(revolveProfileCombo_);
    revolveProfileCombo_->setCurrentIndex(
        revolveProfileCombo_->findData(QVariant::fromValue<qulonglong>(id)));
  }
  rebuildRevolveAxisChoices();
  const auto historyEntry = std::find_if(
      sketchViews_.begin(), sketchViews_.end(),
      [id](const SketchViewEntry& entry) {
        return entry.documentSketchId == id;
      });
  if (historyEntry == sketchViews_.end()) return;
  const std::size_t sketchIndex = static_cast<std::size_t>(
      std::distance(sketchViews_.begin(), historyEntry));
  viewport_->beginRevolveAxisSelection(sketchIndex);
  const std::size_t count = viewport_->selectedProfileRegionCount();
  revolveProfileSummary_->setText(
      QString::fromUtf8("Выбрано областей: %1. Ctrl — добавить или убрать.")
          .arg(count));
  updateRevolveToolPreview();
  statusBar()->showMessage(QString::fromUtf8(
      "Профили выбраны. Ctrl добавляет области; щёлкните ось или прямую эскиза."));
}

void MainWindow::rebuildRevolveAxisChoices() {
  const auto sketchId = static_cast<SketchId>(
      revolveProfileCombo_->currentData().toULongLong());
  const QSignalBlocker blocker(revolveAxisCombo_);
  revolveAxisCombo_->clear();
  revolveAxisCombo_->addItem(QString::fromUtf8("Выбрать ось"), 0);
  revolveAxisCombo_->addItem(QStringLiteral("Global X"),
                             QVariant::fromValue<qulonglong>(Viewport::kGlobalXAxisToken));
  revolveAxisCombo_->addItem(QStringLiteral("Global Y"),
                             QVariant::fromValue<qulonglong>(Viewport::kGlobalYAxisToken));
  revolveAxisCombo_->addItem(QStringLiteral("Global Z"),
                             QVariant::fromValue<qulonglong>(Viewport::kGlobalZAxisToken));
  if (sketchId == kInvalidSketchId) return;
  revolveAxisCombo_->addItem(QString::fromUtf8("Горизонтальная ось"), 1);
  revolveAxisCombo_->addItem(QString::fromUtf8("Вертикальная ось"), 2);
  const auto* sketch = activePartDocument().findSketch(sketchId);
  if (!sketch) return;
  for (std::size_t i = 0; i < sketch->geometry.lines().size(); ++i)
    revolveAxisCombo_->addItem(QString::fromUtf8("Линия %1").arg(i + 1),
        QVariant::fromValue<qulonglong>(sketch->geometry.lineId(i) + 3));
}

void MainWindow::updateRevolveToolPreview() {
  if (!partProjectActive()) return;
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle == ToolLifecycle::Inactive) return;
  const bool valid = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle == ToolLifecycle::PreviewValid;
  revolveAcceptButton_->setEnabled(valid);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).previewShape) {
    viewport_->setToolPreviewPresentation(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).extrudeOperation == ExtrudeOperation::NewBody
            ? ToolPreviewPresentation::OverlaySourceSelection
            : ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).bodyId,
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).sourceFeatureId, partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).previewShape);
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).extrudeOperation != ExtrudeOperation::NewBody)
      viewport_->setToolPreviewReplacedBodies(
          {partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).bodyId});
  } else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).angularManipulator)
    viewport_->setAngularToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle == ToolLifecycle::PreviewInvalid) {
    revolveStepHint_->setText(QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).error));
    revolveStepHint_->setProperty("uiRole", "danger");
    statusBar()->showMessage(QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).error));
  } else {
    const QString hint = valid
        ? partDesignToolStepHint(PartDesignToolKind::Revolve,
                                 ToolSelectionStage::EditingParameters)
        : partDesignToolStepHint(PartDesignToolKind::Revolve,
                                 partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).selectionStage);
    revolveStepHint_->setText(QString::fromUtf8("Сейчас: ") + hint);
    revolveStepHint_->setProperty("uiRole", "secondaryText");
  }
  revolveStepHint_->style()->unpolish(revolveStepHint_);
  revolveStepHint_->style()->polish(revolveStepHint_);
}

void MainWindow::cancelRevolveTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptRevolveTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle != ToolLifecycle::PreviewValid ||
      !partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).axisReference) return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previous = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  Body* body = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, mutableStandaloneDocument()).editingFeatureId
                   ? mutableStandaloneDocument().findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).bodyId)
                   : partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).extrudeOperation == ExtrudeOperation::NewBody
                         ? &mutableStandaloneDocument().addBody()
                         : mutableStandaloneDocument().activeBody();
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* feature = dynamic_cast<RevolveFeature*>(body->features()[index].get());
      if (!feature || feature->id() != *editingId) continue;
      feature->setProfileSketchId(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileSketchId);
      feature->setProfileOverride(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileOverride);
      feature->setAxis(*partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).axisReference);
      feature->setAngleDeg(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).angleDeg);
      feature->setOperation(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).extrudeOperation);
      feature->setReversed(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).reversed);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    auto feature = std::make_unique<RevolveFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileSketchId, *partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).axisReference,
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).angleDeg,
        "Revolve " + std::to_string(body->features().size() + 1),
        partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).extrudeOperation, partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).reversed);
    feature->setProfileOverride(partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).profileOverride);
    body->addFeature(std::move(feature));
  }
  if (!mutableStandaloneDocument().recompute()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = previous; refreshBodyViewFromDocument();
    statusBar()->showMessage(error); return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree(); rebuildHistoryPanel();
  completeModelTransitionUi();
}

void MainWindow::createPocket() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  Body* body = mutableStandaloneDocument().activeBody();
  if (!body || !body->resultShape() || sketchViews_.empty()) {
    QMessageBox::information(this, QString::fromUtf8("Карман"),
                             QString::fromUtf8("Сначала создайте Body и эскиз на его грани."));
    return;
  }
  const auto& historySketch = sketchViews_.back();
  DocumentSketch* profile = mutableStandaloneDocument().findSketch(historySketch.documentSketchId);
  if (!profile || profile->support.type != SketchSupportType::Face ||
      !profile->supportResolved || !profile->geometry.isClosed()) {
    QMessageBox::warning(this, QString::fromUtf8("Карман"),
                         QString::fromUtf8("Нужен замкнутый эскиз на разрешённой плоской грани."));
    return;
  }
  bool accepted = false;
  const double depth = QInputDialog::getDouble(
      this, QString::fromUtf8("Создать карман"),
      QString::fromUtf8("Глубина, мм:"), 20.0, 0.01, 100000.0, 2, &accepted);
  if (!accepted) return;
  static_cast<void>(acceptPocketTransition(profile->id, depth));
}

bool MainWindow::acceptPocketTransition(
    SketchId profileId, double depthMm,
    std::optional<FeatureId> editingFeatureId) {
  if (!partProjectActive()) return false;
  Body* body = mutableStandaloneDocument().activeBody();
  if (editingFeatureId)
    for (const auto& candidate : activePartDocument().bodies())
      if (candidate.featureIndex(*editingFeatureId)) {
        body = mutableStandaloneDocument().findBody(candidate.id());
        break;
      }
  if (!body || !activePartDocument().findSketch(profileId) || depthMm <= 0.0)
    return false;
  Document previousDocument = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (editingFeatureId) {
    auto* pocket = dynamic_cast<PocketFeature*>(
        mutableStandaloneDocument().findFeature(*editingFeatureId));
    if (!pocket) return false;
    pocket->setDepthMm(depthMm);
  } else {
    body->addFeature(std::make_unique<PocketFeature>(
        profileId, depthMm,
        "Pocket " + std::to_string(body->features().size())));
  }
  if (!mutableStandaloneDocument().rebuild()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = std::move(previousDocument);
    refreshBodyViewFromDocument();
    statusBar()->showMessage(error, 4000);
    return false;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return false;
  refreshBodyViewFromDocument();
  modelRibbon_->clearActiveTool();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(
      editingFeatureId
          ? QString::fromUtf8("Глубина кармана изменена: %1 мм").arg(depthMm)
          : QString::fromUtf8("Создан карман глубиной %1 мм").arg(depthMm),
      editingFeatureId ? 3000 : 4000);
  return true;
}

void MainWindow::createFillet() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  auto edges = viewport_->selectedBodyEdges();
  Body* body = edges.empty() ? mutableStandaloneDocument().activeBody()
                             : mutableStandaloneDocument().findBody(edges.front().bodyId());
  if (!body || !body->activeFeature() || !body->resultShape()) {
    QMessageBox::information(this, QString::fromUtf8("Скругление"),
                             QString::fromUtf8("Сначала создайте тело."));
    return;
  }
  if (!edges.empty() &&
      body->activeFeature()->id() != edges.front().featureId())
    edges.clear();

  for (const auto& edge : edges)
    if (edge.bodyId() != body->id() ||
        edge.featureId() != body->activeFeature()->id()) {
      QMessageBox::warning(this, QString::fromUtf8("Скругление"),
                           QString::fromUtf8("Рёбра должны принадлежать одному телу"));
      return;
    }
  const auto sourceSnapshot =
      body->topologySnapshot(body->activeFeature()->id());
  if (!sourceSnapshot) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginFillet(*sourceSnapshot, edges, 0.0),
          PartDesignToolKind::Fillet))
    return;
  viewport_->setEdgeMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Edge);
  viewport_->setSelectedBodyEdges(edges);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Fillet),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Радиус"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.0, 100000.0, 2);
  toolParametersPanel_->setParameterValue(0.0);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFilletToolPreview();
  if (!edges.empty())
    static_cast<void>(viewport_->focusToolParameterField(false));
  if (edges.empty())
    statusBar()->showMessage(
        QString::fromUtf8("Нажмите «Выбрать» и укажите рёбра в viewport"));
}

void MainWindow::editPatternFeature(FeatureId featureId,
                                    FeatureEditorRoute route) {
  if (!partProjectActive()) return;
  Body* owner = mutableStandaloneDocument().findBodyForFeature(featureId);
  ShapeFeature* feature = mutableStandaloneDocument().findFeature(featureId);
  const auto featureIndex =
      owner ? owner->featureIndex(featureId) : std::nullopt;
  if (!owner || !feature || !featureIndex) return;
  Body& body = *owner;
  const std::size_t index = *featureIndex;
      if (route == FeatureEditorRoute::Revolve) {
        auto* revolve = dynamic_cast<RevolveFeature*>(feature);
        if (!revolve) return;
        ShapeFeature::ShapePtr upstream = index == 0
                                             ? ShapeFeature::ShapePtr{}
                                             : body.features()[index - 1]->shape();
        const FeatureId sourceId = index == 0
                                       ? kInvalidFeatureId
                                       : body.features()[index - 1]->id();
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginRevolve(
                    activePartDocument(), body.id(), sourceId, upstream, revolve->id()),
                PartDesignToolKind::Revolve))
          return;
        partDesignCoordinator_.setRevolveProfile(activePartDocument(), revolve->profileSketchId(),
                                       revolve->profileOverride());
        partDesignCoordinator_.setRevolveAxis(activePartDocument(), revolve->axis());
        partDesignCoordinator_.setRevolveAngleFromPanel(activePartDocument(), revolve->angleDeg());
        partDesignCoordinator_.setRevolveOperation(activePartDocument(), revolve->operation());
        partDesignCoordinator_.setRevolveReversed(activePartDocument(), revolve->reversed());
        {
          const QSignalBlocker profileBlocker(revolveProfileCombo_);
          const QSignalBlocker angleBlocker(revolveAngleSpin_);
          const QSignalBlocker operationBlocker(revolveOperationCombo_);
          const QSignalBlocker reverseBlocker(revolveReverseCheck_);
          revolveProfileCombo_->setCurrentIndex(revolveProfileCombo_->findData(
              QVariant::fromValue<qulonglong>(revolve->profileSketchId())));
          revolveAngleSpin_->setValue(revolve->angleDeg());
          revolveOperationCombo_->setCurrentIndex(
              static_cast<int>(revolve->operation()));
          revolveReverseCheck_->setChecked(revolve->reversed());
        }
        revolveProfileSummary_->setText(
            revolve->profileOverride()
                ? QString::fromUtf8("Сохранён выбранный набор областей")
                : QString::fromUtf8("Выбран весь эскиз"));
        rebuildRevolveAxisChoices();
        qulonglong axisToken = 0;
        switch (revolve->axis().type) {
          case AxisReferenceType::GlobalX: axisToken = Viewport::kGlobalXAxisToken; break;
          case AxisReferenceType::GlobalY: axisToken = Viewport::kGlobalYAxisToken; break;
          case AxisReferenceType::GlobalZ: axisToken = Viewport::kGlobalZAxisToken; break;
          case AxisReferenceType::SketchHorizontalAxis: axisToken = 1; break;
          case AxisReferenceType::SketchVerticalAxis: axisToken = 2; break;
          case AxisReferenceType::SketchLine:
            axisToken = static_cast<qulonglong>(revolve->axis().lineId) + 3;
            break;
        }
        {
          const QSignalBlocker axisBlocker(revolveAxisCombo_);
          revolveAxisCombo_->setCurrentIndex(revolveAxisCombo_->findData(axisToken));
        }
        updateRevolveToolPreview();
        revolveDock_->show();
        revolveDock_->raise();
        statusBar()->showMessage(QString::fromUtf8(
            "Редактирование вращения: измените угол или выберите ось"));
        return;
      }
      if (route == FeatureEditorRoute::Shell) {
        auto* shell = dynamic_cast<ShellFeature*>(feature);
        if (!shell) return;
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginShell(
                    body.id(), body.features()[index - 1]->id(), source,
                    shell->removedFaces(), shell->thicknessMm(),
                    shell->outside(), shell->id(),
                    body.features()[index - 1]->topologyIndex()),
                PartDesignToolKind::Shell))
          return;
        viewport_->setFaceMultiSelectionMode(true);
        viewport_->setSelectionFilter(SelectionFilter::Face);
        viewport_->setSelectedBodyFaces(shell->removedFaces());
        toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Shell),
            QString::fromUtf8("Удаляемые грани"), QString::fromUtf8("Толщина"),
            QStringLiteral(" mm"));
        toolParametersPanel_->setParameterRange(0.01, 100000.0, 2);
        toolParametersPanel_->setParameterValue(shell->thicknessMm());
        toolParametersPanel_->configureOption(QString::fromUtf8("Наружу"),
                                               shell->outside());
        toolParametersDock_->show(); toolParametersDock_->raise();
        updateShellToolPreview();
        return;
      }
      if (route == FeatureEditorRoute::Draft) {
        auto* draft = dynamic_cast<DraftFeature*>(feature);
        if (!draft) return;
        if (index == 0) return;
        const auto sourceSnapshot =
            body.topologySnapshot(body.features()[index - 1]->id());
        if (!sourceSnapshot) return;
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginDraft(
                    activePartDocument(), *sourceSnapshot, draft->draftedFaces(), draft->neutralPlane(),
                    draft->pullDirection(), draft->angleDeg(),
                    draft->reversed(), draft->id(), draft->rotationEdge()),
                PartDesignToolKind::Draft))
          return;
        viewport_->setFaceMultiSelectionMode(false);
        viewport_->setSelectionFilter(SelectionFilter::Face);
        viewport_->setSelectedBodyFaces(draft->draftedFaces());
        toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Draft),
            QString::fromUtf8("Поверхность"), QString::fromUtf8("Угол"),
            QString::fromUtf8("°"));
        toolParametersPanel_->setParameterRange(-89.99, 89.99, 2);
        toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).angleDeg);
        toolParametersDock_->show(); toolParametersDock_->raise();
        if (const auto axis = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).principalAxisIndex)
          viewport_->showDraftAxisSelection(*axis);
        else if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).rotationEdge)
          viewport_->showDraftEdgeAxisSelection(
              *partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).rotationEdge);
        updateDraftToolPreview();
        static_cast<void>(viewport_->focusToolParameterField(false));
        return;
      }
      if (route == FeatureEditorRoute::Move) {
        auto* move = dynamic_cast<MoveFeature*>(feature);
        if (!move) return;
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!source) return;
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginMove(move->offsetMm(), move->id()),
                PartDesignToolKind::Move))
          return;
        partDesignCoordinator_.setMoveBody(body.id(), body.features()[index - 1]->id(),
                                 source);
        moveBodyValue_->setText(QString::fromStdString(body.name()));
        moveBodySelectButton_->setEnabled(false);
        {
          const QSignalBlocker xBlocker(moveXSpin_);
          const QSignalBlocker yBlocker(moveYSpin_);
          const QSignalBlocker zBlocker(moveZSpin_);
          const auto offset = move->offsetMm();
          moveXSpin_->setValue(offset.x);
          moveYSpin_->setValue(offset.y);
          moveZSpin_->setValue(offset.z);
        }
        viewport_->setSelectedBodies({body.id()});
        viewport_->showMovePreview();
        updateMoveToolPreview();
        moveDock_->show();
        moveDock_->raise();
        statusBar()->showMessage(
            QString::fromUtf8("Редактирование перемещения: потяните стрелку или задайте координаты"));
        return;
      }
      if (route == FeatureEditorRoute::Mirror) {
        auto* mirror = dynamic_cast<MirrorFeature*>(feature);
        if (!mirror) return;
        if (index == 0) return;
        const auto source = body.features()[index - 1]->shape();
        if (!source) return;
        mirrorPresentationBefore_ = editUiTransaction_
            ? std::optional<EditorPresentationState>{
                  editUiTransaction_->before.presentation}
            : std::optional<EditorPresentationState>{
                  captureHistorySelection().presentation};
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginMirror(mirror->id()),
                PartDesignToolKind::Mirror))
          return;
        partDesignCoordinator_.setMirrorBody(body.id(), body.features()[index - 1]->id(),
                                   source,
                                   body.features()[index - 1]->topologyIndex());
        if (mirror->planeFace())
          partDesignCoordinator_.setMirrorPlaneFace(*mirror->planeFace());
        else
          partDesignCoordinator_.setMirrorPlane(mirror->plane());
        mirrorBodyValue_->setText(QString::fromStdString(body.name()));
        if (mirror->planeFace())
          mirrorPlaneValue_->setText(
              mirror->planeFace()->isV1()
                  ? QString::fromUtf8("Грань #%1")
                        .arg(mirror->planeFace()->faceIndex() + 1)
                  : QString::fromUtf8("Грань (устойчивая ссылка)"));
        else
          mirrorPlaneValue_->setText(
              mirror->plane() == MirrorPlane::XY
                  ? QStringLiteral("XY")
                  : mirror->plane() == MirrorPlane::XZ
                        ? QStringLiteral("XZ")
                        : QStringLiteral("YZ"));
        // A Mirror feature belongs to its Body; editing changes the reference
        // plane only. Body re-selection is intentionally available on creation
        // and disabled here to avoid moving a history feature across Bodies.
        mirrorBodySelectButton_->setEnabled(false);
        mirrorPlaneSelectButton_->setEnabled(true);
        viewport_->setSelectedBodies({body.id()});
        if (mirror->planeFace())
          viewport_->showMirrorFaceSelection(*mirror->planeFace());
        else
          viewport_->showMirrorPlaneSelection(
              static_cast<int>(mirror->plane()));
        updateMirrorToolPreview();
        mirrorDock_->show();
        mirrorDock_->raise();
        statusBar()->showMessage(
            QString::fromUtf8(
                "Редактирование зеркала: выберите плоскость или плоскую грань в 3D-виде"));
        return;
      }
      if (route == FeatureEditorRoute::LinearPattern) {
        auto* linear = dynamic_cast<LinearPatternFeature*>(feature);
        if (!linear) return;
        Body* sourceBody = &body;
        ShapeFeature::ShapePtr source;
        FeatureId sourceFeatureId = linear->sourceFeatureId();
        if (linear->operation() == PatternOperation::Join) {
          if (index == 0) return;
          source = body.features()[index - 1]->shape();
          sourceFeatureId = body.features()[index - 1]->id();
        } else {
          sourceBody = mutableStandaloneDocument().findBody(linear->sourceBodyId());
          if (sourceBody)
            for (const auto& candidate : sourceBody->features())
              if (candidate->id() == linear->sourceFeatureId()) {
                source = candidate->shape();
                break;
              }
        }
        if (!sourceBody) return;
        if (!source) return;
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginLinearPattern(
                    linear->spacingMm(), linear->count(), linear->operation(),
                    linear->id()),
                PartDesignToolKind::LinearPattern))
          return;
        partDesignCoordinator_.setLinearPatternBody(
            sourceBody->id(), sourceFeatureId, source);
        partDesignCoordinator_.setLinearPatternDirection(linear->direction());
        linearPatternBodyValue_->setText(
            QString::fromStdString(sourceBody->name()));
        linearPatternAxisValue_->setText(
            linear->direction() == PrincipalAxis::X
                ? QStringLiteral("X")
                : linear->direction() == PrincipalAxis::Y
                      ? QStringLiteral("Y")
                      : QStringLiteral("Z"));
        {
          const QSignalBlocker spacingBlocker(linearPatternSpacingSpin_);
          const QSignalBlocker countBlocker(linearPatternCountSpin_);
          const QSignalBlocker operationBlocker(linearPatternOperationCombo_);
          linearPatternSpacingSpin_->setValue(linear->spacingMm());
          linearPatternCountSpin_->setValue(linear->count());
          linearPatternOperationCombo_->setCurrentIndex(
              linear->operation() == PatternOperation::NewBody ? 0 : 1);
        }
        // The feature stays in its owning Body while editing; only direction
        // and numeric parameters may be changed.
        linearPatternBodySelectButton_->setEnabled(false);
        linearPatternAxisSelectButton_->setEnabled(true);
        linearPatternOperationCombo_->setEnabled(false);
        viewport_->setSelectedBodies({sourceBody->id()});
        viewport_->showLinearPatternAxisSelection(
            static_cast<int>(linear->direction()));
        updateLinearPatternToolPreview();
        linearPatternDock_->show();
        linearPatternDock_->raise();
        statusBar()->showMessage(QString::fromUtf8(
            "Редактирование линейного массива: выберите ось или задайте параметры"));
        return;
      }
      if (route == FeatureEditorRoute::CircularPattern) {
        auto* circular = dynamic_cast<CircularPatternFeature*>(feature);
        if (!circular) return;
        Body* sourceBody = &body;
        ShapeFeature::ShapePtr source;
        FeatureId sourceFeatureId = circular->sourceFeatureId();
        if (circular->operation() == PatternOperation::Join) {
          if (index == 0) return;
          source = body.features()[index - 1]->shape();
          sourceFeatureId = body.features()[index - 1]->id();
        } else {
          sourceBody = mutableStandaloneDocument().findBody(circular->sourceBodyId());
          if (sourceBody)
            for (const auto& candidate : sourceBody->features())
              if (candidate->id() == circular->sourceFeatureId()) {
                source = candidate->shape();
                break;
              }
        }
        if (!sourceBody) return;
        if (!source) return;
        if (!applyPartDesignBeginResult(
                partDesignCoordinator_.beginCircularPattern(
                    circular->angleDeg(), circular->count(),
                    circular->operation(), circular->id()),
                PartDesignToolKind::CircularPattern))
          return;
        partDesignCoordinator_.setCircularPatternBody(
            sourceBody->id(), sourceFeatureId, source);
        partDesignCoordinator_.setCircularPatternAxis(circular->axis());
        circularPatternBodyValue_->setText(
            QString::fromStdString(sourceBody->name()));
        circularPatternAxisValue_->setText(
            circular->axis() == PrincipalAxis::X
                ? QStringLiteral("X")
                : circular->axis() == PrincipalAxis::Y
                      ? QStringLiteral("Y")
                      : QStringLiteral("Z"));
        {
          const QSignalBlocker angleBlocker(circularPatternAngleSpin_);
          const QSignalBlocker countBlocker(circularPatternCountSpin_);
          const QSignalBlocker operationBlocker(circularPatternOperationCombo_);
          circularPatternAngleSpin_->setValue(circular->angleDeg());
          circularPatternCountSpin_->setValue(circular->count());
          circularPatternOperationCombo_->setCurrentIndex(
              circular->operation() == PatternOperation::NewBody ? 0 : 1);
        }
        circularPatternBodySelectButton_->setEnabled(false);
        circularPatternAxisSelectButton_->setEnabled(true);
        circularPatternOperationCombo_->setEnabled(false);
        viewport_->setSelectedBodies({sourceBody->id()});
        viewport_->showCircularPatternAxisSelection(
            static_cast<int>(circular->axis()));
        updateCircularPatternToolPreview();
        circularPatternDock_->show();
        circularPatternDock_->raise();
        statusBar()->showMessage(QString::fromUtf8(
            "Редактирование кругового массива: выберите ось или задайте параметры"));
        return;
      }
      return;
}

void MainWindow::createMove() {
  if (!moveCommandContextActive()) return;
  if (standalonePartProjectActive() && !ensureHistoryAtEnd()) return;
  if (definitionEditContext_ && assemblyCommandHistory_.isBusy()) return;
  resetTransientModelingUi();
  const bool hasBody = std::any_of(
      activePartDocument().bodies().begin(), activePartDocument().bodies().end(),
      [](const Body& body) { return body.activeFeature() && body.resultShape(); });
  if (!hasBody) {
    QMessageBox::information(this, QString::fromUtf8("Перемещение"),
                             QString::fromUtf8("Сначала создайте тело."));
    modelRibbon_->clearActiveTool();
    return;
  }
  if (!applyPartDesignBeginResult(partDesignCoordinator_.beginMove(),
                                  PartDesignToolKind::Move))
    return;
  moveBodyValue_->setText(QString::fromUtf8("Не выбрано"));
  moveBodySelectButton_->setEnabled(true);
  moveAcceptButton_->setEnabled(false);
  {
    const QSignalBlocker xBlocker(moveXSpin_);
    const QSignalBlocker yBlocker(moveYSpin_);
    const QSignalBlocker zBlocker(moveZSpin_);
    moveXSpin_->setValue(0.0);
    moveYSpin_->setValue(0.0);
    moveZSpin_->setValue(0.0);
  }
  moveDock_->show();
  moveDock_->raise();
  viewport_->beginMoveBodySelection();
  updateMoveToolPreview();
  statusBar()->showMessage(QString::fromUtf8("Выберите тело в 3D-виде"));
}

void MainWindow::updateMoveToolPreview() {
  if (!moveCommandContextActive()) return;
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const bool valid =
      partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::PreviewValid;
  moveAcceptButton_->setEnabled(valid);
  if (valid && partDesignCoordinator_.snapshot(PartDesignToolKind::Move).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Move).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Move).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Move).bodyId});
    if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Move).translationManipulator)
      viewport_->setTranslationToolManipulator(*manipulator);
    moveStepHint_->setText(QString::fromUtf8(
        "Потяните цветную стрелку X, Y или Z либо задайте точные смещения."));
    moveStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed =
        partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::PreviewInvalid;
    moveStepHint_->setText(
        failed ? QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::Move).error)
               : QString::fromUtf8("Сейчас: ") +
                     partDesignToolStepHint(PartDesignToolKind::Move,
                                            partDesignCoordinator_.snapshot(PartDesignToolKind::Move).selectionStage));
    moveStepHint_->setProperty("uiRole",
                               failed ? "danger" : "secondaryText");
  }
  moveStepHint_->style()->unpolish(moveStepHint_);
  moveStepHint_->style()->polish(moveStepHint_);
}

void MainWindow::cancelMoveTool() {
  if (!moveCommandContextActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptMoveTool() {
  if (!moveCommandContextActive()) return;
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle != ToolLifecycle::PreviewValid) return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  const auto snapshot =
      partDesignCoordinator_.snapshot(PartDesignToolKind::Move);
  auto applyMove = [&snapshot](Document& target) {
    Body* body = target.findBody(snapshot.bodyId);
    if (!body) return false;
    if (const auto editingId = snapshot.editingFeatureId) {
      bool found = false;
      for (std::size_t index = 0; index < body->features().size(); ++index) {
        auto* move = dynamic_cast<MoveFeature*>(body->features()[index].get());
        if (!move || move->id() != *editingId) continue;
        move->setOffsetMm(snapshot.offsetMm);
        body->markDirtyFrom(index);
        found = true;
        break;
      }
      if (!found) return false;
    } else {
      if (!body->activeFeature() ||
          body->activeFeature()->id() != snapshot.sourceFeatureId)
        return false;
      body->addFeature(std::make_unique<MoveFeature>(
          snapshot.sourceFeatureId, snapshot.offsetMm,
          "Move " + std::to_string(body->features().size() + 1)));
    }
    return true;
  };

  if (definitionEditContext_) {
    Document candidate = activePartDocument();
    if (!applyMove(candidate)) {
      moveStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      moveStepHint_->setProperty("uiRole", "danger");
      return;
    }
    if (definitionMoveRecomputeFailureForTesting_) {
      if (Body* body = candidate.findBody(snapshot.bodyId))
        body->addFeature(std::make_unique<ImportedShapeFeature>(
            std::shared_ptr<const TopoDS_Shape>{},
            "Definition move recompute failure seam"));
    }
    if (!candidate.recompute()) {
      moveStepHint_->setText(
          QString::fromStdString(candidate.rebuildError()));
      moveStepHint_->setProperty("uiRole", "danger");
      moveStepHint_->style()->unpolish(moveStepHint_);
      moveStepHint_->style()->polish(moveStepHint_);
      return;
    }
    if (!commitDefinitionCandidate(std::move(candidate))) return;
    applyPartDesignUiEffect(commit.accept());
    statusBar()->showMessage(
        QString::fromUtf8("Перемещение определения применено"), 3000);
    return;
  }

  Document& live = mutableStandaloneDocument();
  Document previous = live;
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (!applyMove(live)) {
    moveStepHint_->setText(QString::fromUtf8(
        "Исходное тело изменилось. Выберите его заново."));
    moveStepHint_->setProperty("uiRole", "danger");
    return;
  }
  if (!live.recompute()) {
    const QString error = QString::fromStdString(live.rebuildError());
    live = std::move(previous);
    refreshBodyViewFromDocument();
    moveStepHint_->setText(error);
    moveStepHint_->setProperty("uiRole", "danger");
    moveStepHint_->style()->unpolish(moveStepHint_);
    moveStepHint_->style()->polish(moveStepHint_);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Перемещение применено"), 3000);
}

void MainWindow::createMirror() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  const bool hasBody = std::any_of(
      activePartDocument().bodies().begin(), activePartDocument().bodies().end(),
      [](const Body& body) { return body.activeFeature() && body.resultShape(); });
  if (!hasBody) {
    QMessageBox::information(this, QString::fromUtf8("Зеркало"),
                             QString::fromUtf8("Сначала создайте тело."));
    modelRibbon_->clearActiveTool();
    return;
  }
  mirrorPresentationBefore_ = captureHistorySelection().presentation;
  if (!applyPartDesignBeginResult(partDesignCoordinator_.beginMirror(),
                                  PartDesignToolKind::Mirror))
    return;
  mirrorBodyValue_->setText(QString::fromUtf8("Не выбрано"));
  mirrorPlaneValue_->setText(QString::fromUtf8("Не выбрана"));
  mirrorBodySelectButton_->setEnabled(true);
  mirrorPlaneSelectButton_->setEnabled(false);
  mirrorAcceptButton_->setEnabled(false);
  mirrorDock_->show();
  mirrorDock_->raise();
  viewport_->beginMirrorBodySelection();
  updateMirrorToolPreview();
  statusBar()->showMessage(QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
}

void MainWindow::updateMirrorToolPreview() {
  if (!partProjectActive()) return;
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::Inactive) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const bool valid =
      partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::PreviewValid;
  mirrorAcceptButton_->setEnabled(valid);
  if (valid && partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).bodyId});
    mirrorStepHint_->setText(QString::fromUtf8(
        "Предпросмотр построен. Нажмите «Применить» для создания зеркала."));
    mirrorStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    const bool failed =
        partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::PreviewInvalid;
    mirrorStepHint_->setText(
        failed
            ? QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).error)
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(PartDesignToolKind::Mirror,
                                         partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).selectionStage));
    mirrorStepHint_->setProperty("uiRole",
                                 failed ? "danger" : "secondaryText");
  }
  mirrorStepHint_->style()->unpolish(mirrorStepHint_);
  mirrorStepHint_->style()->polish(mirrorStepHint_);
}

void MainWindow::cancelMirrorTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptMirrorTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  const auto snapshot =
      partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror);
  if (snapshot.lifecycle != ToolLifecycle::PreviewValid ||
      (!snapshot.mirrorPlane && !snapshot.mirrorPlaneFace))
    return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Body* body = mutableStandaloneDocument().findBody(snapshot.bodyId);
  if (!body) return;
  Document previous = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = snapshot.editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* mirror =
          dynamic_cast<MirrorFeature*>(body->features()[index].get());
      if (!mirror || mirror->id() != *editingId) continue;
      if (snapshot.mirrorPlaneFace)
        mirror->setPlaneFace(*snapshot.mirrorPlaneFace);
      else
        mirror->setPlane(*snapshot.mirrorPlane);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    if (!body->activeFeature() ||
        body->activeFeature()->id() != snapshot.sourceFeatureId) {
      mirrorStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      mirrorStepHint_->setProperty("uiRole", "danger");
      return;
    }
    const std::string name =
        "Mirror " + std::to_string(body->features().size() + 1);
    if (snapshot.mirrorPlaneFace)
      body->addFeature(std::make_unique<MirrorFeature>(
          snapshot.sourceFeatureId, *snapshot.mirrorPlaneFace, name));
    else
      body->addFeature(std::make_unique<MirrorFeature>(
          snapshot.sourceFeatureId, *snapshot.mirrorPlane, name));
  }
  if (!mutableStandaloneDocument().recompute()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = previous;
    refreshBodyViewFromDocument();
    mirrorStepHint_->setText(error);
    mirrorStepHint_->setProperty("uiRole", "danger");
    mirrorStepHint_->style()->unpolish(mirrorStepHint_);
    mirrorStepHint_->style()->polish(mirrorStepHint_);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  const auto presentationBefore = mirrorPresentationBefore_;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  if (presentationBefore) applyPresentationState(*presentationBefore);
  mirrorPresentationBefore_.reset();
  statusBar()->showMessage(QString::fromUtf8("Зеркало применено"), 3000);
}

void MainWindow::createLinearPattern() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  const bool hasBody = std::any_of(
      activePartDocument().bodies().begin(), activePartDocument().bodies().end(),
      [](const Body& body) { return body.activeFeature() && body.resultShape(); });
  if (!hasBody) {
    QMessageBox::information(this, QString::fromUtf8("Линейный массив"),
                             QString::fromUtf8("Сначала создайте тело."));
    modelRibbon_->clearActiveTool();
    return;
  }
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginLinearPattern(30.0, 3),
          PartDesignToolKind::LinearPattern))
    return;
  linearPatternBodyValue_->setText(QString::fromUtf8("Не выбрано"));
  linearPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
  linearPatternBodySelectButton_->setEnabled(true);
  linearPatternAxisSelectButton_->setEnabled(false);
  linearPatternOperationCombo_->setEnabled(true);
  linearPatternAcceptButton_->setEnabled(false);
  {
    const QSignalBlocker spacingBlocker(linearPatternSpacingSpin_);
    const QSignalBlocker countBlocker(linearPatternCountSpin_);
    const QSignalBlocker operationBlocker(linearPatternOperationCombo_);
    linearPatternSpacingSpin_->setValue(30.0);
    linearPatternCountSpin_->setValue(3);
    linearPatternOperationCombo_->setCurrentIndex(0);
  }
  linearPatternDock_->show();
  linearPatternDock_->raise();
  viewport_->beginLinearPatternBodySelection();
  updateLinearPatternToolPreview();
  statusBar()->showMessage(QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
}

void MainWindow::updateLinearPatternToolPreview() {
  if (!partProjectActive()) return;
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle == ToolLifecycle::Inactive) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const bool valid =
      partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle == ToolLifecycle::PreviewValid;
  linearPatternAcceptButton_->setEnabled(valid);
  if (valid && partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).previewShape);
    viewport_->setToolPreviewReplacedBodies(
        {partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId});
    if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).linearManipulator)
      viewport_->setToolManipulator(*manipulator);
    linearPatternStepHint_->setText(QString::fromUtf8(
        "Потяните стрелку для изменения шага или задайте шаг и количество числом."));
    linearPatternStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed = partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle ==
                        ToolLifecycle::PreviewInvalid;
    linearPatternStepHint_->setText(
        failed
            ? QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).error)
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(
                      PartDesignToolKind::LinearPattern,
                      partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).selectionStage));
    linearPatternStepHint_->setProperty("uiRole",
                                        failed ? "danger" : "secondaryText");
  }
  linearPatternStepHint_->style()->unpolish(linearPatternStepHint_);
  linearPatternStepHint_->style()->polish(linearPatternStepHint_);
}

void MainWindow::cancelLinearPatternTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptLinearPatternTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle != ToolLifecycle::PreviewValid ||
      !partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).principalDirection)
    return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Body* sourceBody = mutableStandaloneDocument().findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).bodyId);
  if (!sourceBody) return;
  Document previous = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).editingFeatureId) {
    auto* linear =
        dynamic_cast<LinearPatternFeature*>(mutableStandaloneDocument().findFeature(*editingId));
    if (!linear) return;
    linear->setDirection(*partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).principalDirection);
    linear->setSpacingMm(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).spacingMm);
    linear->setCount(partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).count);
  } else {
    if (!sourceBody->activeFeature() ||
        sourceBody->activeFeature()->id() !=
            partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId) {
      linearPatternStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      linearPatternStepHint_->setProperty("uiRole", "danger");
      return;
    }
    const BodyId sourceBodyId = sourceBody->id();
    Body* targetBody = partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).patternOperation ==
                               PatternOperation::NewBody
                           ? &mutableStandaloneDocument().addBody()
                           : sourceBody;
    targetBody->addFeature(std::make_unique<LinearPatternFeature>(
        sourceBodyId, partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).sourceFeatureId,
        *partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).principalDirection,
        partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).count,
        partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).spacingMm,
        partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).patternOperation,
        "Linear Pattern " +
            std::to_string(targetBody->features().size() + 1)));
  }
  if (!mutableStandaloneDocument().recompute()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = previous;
    refreshBodyViewFromDocument();
    linearPatternStepHint_->setText(error);
    linearPatternStepHint_->setProperty("uiRole", "danger");
    linearPatternStepHint_->style()->unpolish(linearPatternStepHint_);
    linearPatternStepHint_->style()->polish(linearPatternStepHint_);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Линейный массив применён"),
                           3000);
}

void MainWindow::createCircularPattern() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  const bool hasBody = std::any_of(
      activePartDocument().bodies().begin(), activePartDocument().bodies().end(),
      [](const Body& body) { return body.activeFeature() && body.resultShape(); });
  if (!hasBody) {
    QMessageBox::information(this, QString::fromUtf8("Круговой массив"),
                             QString::fromUtf8("Сначала создайте тело."));
    modelRibbon_->clearActiveTool();
    return;
  }
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginCircularPattern(
              kMaximumPatternAngleDeg, 4),
          PartDesignToolKind::CircularPattern))
    return;
  circularPatternBodyValue_->setText(QString::fromUtf8("Не выбрано"));
  circularPatternAxisValue_->setText(QString::fromUtf8("Не выбрано"));
  circularPatternBodySelectButton_->setEnabled(true);
  circularPatternAxisSelectButton_->setEnabled(false);
  circularPatternOperationCombo_->setEnabled(true);
  circularPatternAcceptButton_->setEnabled(false);
  {
    const QSignalBlocker angleBlocker(circularPatternAngleSpin_);
    const QSignalBlocker countBlocker(circularPatternCountSpin_);
    const QSignalBlocker operationBlocker(circularPatternOperationCombo_);
    circularPatternAngleSpin_->setValue(kMaximumPatternAngleDeg);
    circularPatternCountSpin_->setValue(4);
    circularPatternOperationCombo_->setCurrentIndex(0);
  }
  circularPatternDock_->show();
  circularPatternDock_->raise();
  viewport_->beginCircularPatternBodySelection();
  updateCircularPatternToolPreview();
  statusBar()->showMessage(QString::fromUtf8("1/2 Выберите тело в 3D-виде"));
}

void MainWindow::updateCircularPatternToolPreview() {
  if (!partProjectActive()) return;
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle == ToolLifecycle::Inactive) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const bool valid = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                     ToolLifecycle::PreviewValid;
  circularPatternAcceptButton_->setEnabled(valid);
  if (valid && partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).previewShape);
    viewport_->setToolPreviewReplacedBodies(
        {partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId});
    if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).angularManipulator)
      viewport_->setAngularToolManipulator(*manipulator);
    circularPatternStepHint_->setText(QString::fromUtf8(
        "Потяните дугу для изменения угла или задайте угол и количество числом."));
    circularPatternStepHint_->setProperty("uiRole", "secondaryText");
  } else {
    viewport_->clearToolPreviewShape();
    viewport_->clearToolManipulator();
    const bool failed = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle ==
                        ToolLifecycle::PreviewInvalid;
    circularPatternStepHint_->setText(
        failed
            ? QString::fromStdString(partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).error)
            : QString::fromUtf8("Сейчас: ") +
                  partDesignToolStepHint(
                      PartDesignToolKind::CircularPattern,
                      partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).selectionStage));
    circularPatternStepHint_->setProperty(
        "uiRole", failed ? "danger" : "secondaryText");
  }
  circularPatternStepHint_->style()->unpolish(circularPatternStepHint_);
  circularPatternStepHint_->style()->polish(circularPatternStepHint_);
}

void MainWindow::cancelCircularPatternTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptCircularPatternTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
          ToolLifecycle::PreviewValid ||
      !partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).principalAxis)
    return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Body* sourceBody = mutableStandaloneDocument().findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).bodyId);
  if (!sourceBody) return;
  Document previous = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).editingFeatureId) {
    auto* circular = dynamic_cast<CircularPatternFeature*>(
        mutableStandaloneDocument().findFeature(*editingId));
    if (!circular) return;
    circular->setAxis(*partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).principalAxis);
    circular->setAngleDeg(partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).angleDeg);
    circular->setCount(partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).count);
  } else {
    if (!sourceBody->activeFeature() ||
        sourceBody->activeFeature()->id() !=
            partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId) {
      circularPatternStepHint_->setText(QString::fromUtf8(
          "Исходное тело изменилось. Выберите его заново."));
      circularPatternStepHint_->setProperty("uiRole", "danger");
      return;
    }
    const BodyId sourceBodyId = sourceBody->id();
    Body* targetBody = partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).patternOperation ==
                               PatternOperation::NewBody
                           ? &mutableStandaloneDocument().addBody()
                           : sourceBody;
    targetBody->addFeature(std::make_unique<CircularPatternFeature>(
        sourceBodyId, partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).sourceFeatureId,
        *partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).principalAxis,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).count,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).angleDeg,
        partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).patternOperation,
        "Circular Pattern " +
            std::to_string(targetBody->features().size() + 1)));
  }
  if (!mutableStandaloneDocument().recompute()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = previous;
    refreshBodyViewFromDocument();
    circularPatternStepHint_->setText(error);
    circularPatternStepHint_->setProperty("uiRole", "danger");
    circularPatternStepHint_->style()->unpolish(circularPatternStepHint_);
    circularPatternStepHint_->style()->polish(circularPatternStepHint_);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Круговой массив применён"),
                           3000);
}

void MainWindow::createChamfer() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  auto edges = viewport_->selectedBodyEdges();
  Body* body = edges.empty() ? mutableStandaloneDocument().activeBody()
                             : mutableStandaloneDocument().findBody(edges.front().bodyId());
  if (!body || !body->activeFeature() || !body->resultShape()) {
    QMessageBox::information(this, QString::fromUtf8("Фаска"),
                             QString::fromUtf8("Сначала создайте тело."));
    return;
  }
  if (!edges.empty() &&
      body->activeFeature()->id() != edges.front().featureId())
    edges.clear();
  for (const auto& edge : edges)
    if (edge.bodyId() != body->id() ||
        edge.featureId() != body->activeFeature()->id()) {
      QMessageBox::warning(this, QString::fromUtf8("Фаска"),
                           QString::fromUtf8("Рёбра должны принадлежать одному телу"));
      return;
    }
  const auto sourceSnapshot =
      body->topologySnapshot(body->activeFeature()->id());
  if (!sourceSnapshot) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginChamfer(*sourceSnapshot, edges, 0.0),
          PartDesignToolKind::Chamfer))
    return;
  viewport_->setEdgeMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Edge);
  viewport_->setSelectedBodyEdges(edges);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Chamfer),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Размер"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.0, 100000.0, 2);
  toolParametersPanel_->setParameterValue(0.0);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateChamferToolPreview();
  if (!edges.empty())
    static_cast<void>(viewport_->focusToolParameterField(false));
  if (edges.empty())
    statusBar()->showMessage(
        QString::fromUtf8("Нажмите «Выбрать» и укажите рёбра в viewport"));
}

void MainWindow::createJoinBodies() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  std::size_t availableBodies = 0;
  for (const Body& body : activePartDocument().bodies())
    if (body.visible() && body.resultShape()) ++availableBodies;
  if (availableBodies < 2) {
    QMessageBox::information(
        this, QString::fromUtf8("Соединить тела"),
        QString::fromUtf8("Для объединения нужны два видимых твёрдых тела."));
    modelRibbon_->clearActiveTool();
    return;
  }
  resetTransientModelingUi();
  if (!applyPartDesignBeginResult(partDesignCoordinator_.beginJoinBodies(),
                                  PartDesignToolKind::JoinBodies))
    return;
  toolParametersPanel_->configureSelectionOnly(
      *partDesignToolHelp(PartDesignToolKind::JoinBodies),
      QString::fromUtf8("Тела"));
  toolParametersPanel_->setSelectionCount(0);
  toolParametersPanel_->setAcceptEnabled(false);
  toolParametersPanel_->setStatus(
      QString::fromUtf8("Выберите первое, затем второе тело"));
  toolParametersDock_->show();
  toolParametersDock_->raise();
  viewport_->beginJoinBodiesSelection();
  statusBar()->showMessage(
      QString::fromUtf8("Соединить тела: выберите два тела в 3D-виде"));
}

void MainWindow::editJoinBodiesStep(BodyId bodyId, FeatureId featureId) {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  auto* join = dynamic_cast<JoinBodiesFeature*>(
      findHistoryFeature(mutableStandaloneDocument(), bodyId, featureId));
  if (!join) return;
  const auto makeInput = [this](BodyId sourceBodyId,
                                FeatureId sourceFeatureId)
      -> std::optional<JoinBodyInput> {
    const Body* sourceBody = mutableStandaloneDocument().findBody(sourceBodyId);
    const auto* source = dynamic_cast<const ShapeFeature*>(
        activePartDocument().findFeature(sourceFeatureId));
    if (!sourceBody || !source || !source->lastValidShape()) return std::nullopt;
    return JoinBodyInput{sourceBodyId, sourceFeatureId,
                         source->lastValidShape()};
  };
  const auto first = makeInput(join->firstBodyId(), join->firstFeatureId());
  const auto second = makeInput(join->secondBodyId(), join->secondFeatureId());
  if (!first || !second) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginJoinBodies(featureId, bodyId),
          PartDesignToolKind::JoinBodies))
    return;
  partDesignCoordinator_.setJoinBodies({*first, *second});
  // The original operands are normally hidden by the committed Join. Expose
  // them only in the viewport presentation so Select can replace either input
  // without mutating committed visibility before the edit is accepted.
  std::vector<BodyId> selectableOperands;
  for (const BodyId operandId : {join->firstBodyId(), join->secondBodyId()})
    if (!bodyConsumedByJoin(activePartDocument(), operandId, featureId))
      selectableOperands.push_back(operandId);
  refreshBodyViewFromDocument(selectableOperands);
  toolParametersPanel_->configureSelectionOnly(
      *partDesignToolHelp(PartDesignToolKind::JoinBodies),
      QString::fromUtf8("Тела"));
  toolParametersDock_->show();
  toolParametersDock_->raise();
  viewport_->beginJoinBodiesSelection();
  viewport_->setSelectedBodies(selectableOperands);
  updateJoinBodiesToolPreview();
  statusBar()->showMessage(
      QString::fromUtf8("Редактирование объединения тел"), 3000);
}

void MainWindow::updateJoinBodiesToolPreview() {
  if (!partProjectActive()) return;
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  QString status;
  bool error = false;
  if (valid) {
    status = QString::fromUtf8("Предпросмотр объединения построен");
  } else if (state == ToolLifecycle::PreviewInvalid) {
    error = true;
    status = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).errorCode ==
                     OperationFailureCode::BodiesDoNotTouch
                 ? QString::fromUtf8(
                       "Тела должны соприкасаться или пересекаться")
                 : QString::fromUtf8("Не удалось объединить выбранные тела");
  } else {
    status = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies.empty()
                 ? QString::fromUtf8("Выберите первое тело")
                 : QString::fromUtf8("Выберите второе тело");
  }
  toolParametersPanel_->setStatus(status, error);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(kInvalidBodyId, kInvalidFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).previewShape);
    std::vector<BodyId> replaced;
    replaced.reserve(partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies.size());
    for (const auto& input : partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies)
      replaced.push_back(input.bodyId);
    viewport_->setToolPreviewReplacedBodies(std::move(replaced));
  } else {
    viewport_->clearToolPreviewShape();
  }
  if (error) statusBar()->showMessage(status, 5000);
}

void MainWindow::cancelJoinBodiesTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptJoinBodiesTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle != ToolLifecycle::PreviewValid ||
      partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies.size() != 2)
    return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  const auto inputs = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).bodies;
  Body* first = mutableStandaloneDocument().findBody(inputs[0].bodyId);
  Body* second = mutableStandaloneDocument().findBody(inputs[1].bodyId);
  auto* firstFeature = dynamic_cast<ShapeFeature*>(
      mutableStandaloneDocument().findFeature(inputs[0].featureId));
  auto* secondFeature = dynamic_cast<ShapeFeature*>(
      mutableStandaloneDocument().findFeature(inputs[1].featureId));
  if (!first || !second || first == second || !firstFeature ||
      !secondFeature || !first->featureIndex(inputs[0].featureId) ||
      !second->featureIndex(inputs[1].featureId) ||
      !firstFeature->lastValidShape() || !secondFeature->lastValidShape()) {
    toolParametersPanel_->setStatus(
        QString::fromUtf8("Исходные тела изменились. Выберите их заново."),
        true);
    return;
  }

  Document previous = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).editingFeatureId) {
    Body* owner = nullptr;
    for (const auto& candidate : activePartDocument().bodies())
      if (candidate.featureIndex(*editingId)) {
        owner = mutableStandaloneDocument().findBody(candidate.id());
        break;
      }
    auto* join = dynamic_cast<JoinBodiesFeature*>(
        mutableStandaloneDocument().findFeature(*editingId));
    if (!owner || !join || owner == first || owner == second) {
      mutableStandaloneDocument() = std::move(previous);
      return;
    }
    const BodyId oldFirst = join->firstBodyId();
    const BodyId oldSecond = join->secondBodyId();
    join->setInputs(inputs[0].bodyId, inputs[0].featureId,
                    inputs[1].bodyId, inputs[1].featureId);
    std::vector<BodyId> detached;
    for (const BodyId oldId : {oldFirst, oldSecond})
      if (oldId != inputs[0].bodyId && oldId != inputs[1].bodyId)
        detached.push_back(oldId);
    releaseUnconsumedJoinOperands(mutableStandaloneDocument(), detached);
  } else {
    Body& resultBody = mutableStandaloneDocument().addBody(
        "Joined Body " + std::to_string(activePartDocument().bodies().size() + 1));
    resultBody.addFeature(std::make_unique<JoinBodiesFeature>(
        inputs[0].bodyId, inputs[0].featureId, inputs[1].bodyId,
        inputs[1].featureId, "Соединение тел"));
  }
  first = mutableStandaloneDocument().findBody(inputs[0].bodyId);
  second = mutableStandaloneDocument().findBody(inputs[1].bodyId);
  first->setVisible(false);
  second->setVisible(false);
  if (!mutableStandaloneDocument().recompute()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }

  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Тела объединены"), 3000);
}

void MainWindow::createShell() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  auto faces = viewport_->selectedBodyFaces();
  Body* body = faces.empty() ? mutableStandaloneDocument().activeBody()
                             : mutableStandaloneDocument().findBody(faces.front().bodyId());
  if (!body || !body->activeFeature() || !body->resultShape()) return;
  if (!faces.empty() && faces.front().featureId() != body->activeFeature()->id())
    faces.clear();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginShell(
              body->id(), body->activeFeature()->id(), body->resultShape(),
              faces, 2.0, false, std::nullopt,
              body->activeFeature()->topologyIndex()),
          PartDesignToolKind::Shell))
    return;
  viewport_->setFaceMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setSelectedBodyFaces(faces);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Shell),
                                  QString::fromUtf8("Удаляемые грани"),
                                  QString::fromUtf8("Толщина"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.01, 100000.0, 2);
  toolParametersPanel_->setParameterValue(2.0);
  toolParametersPanel_->configureOption(QString::fromUtf8("Наружу"), false);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateShellToolPreview();
  // Shell HUD owns CAD Tab focus; do not leave focus on the ribbon button when
  // the operation starts from a preselected face.
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::updateShellToolPreview() {
  if (!partProjectActive()) return;
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).faces.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  const QString limitStatus = QString::fromUtf8(
      "Достигнута предельная толщина: %1 мм")
                                  .arg(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).thicknessMm, 0, 'f', 2);
  toolParametersPanel_->setParameterRange(
      0.01, partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).maximumValidValueMm.value_or(100000.0), 2);
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).thicknessMm);
  toolParametersPanel_->setAcceptEnabled(valid);
  toolParametersPanel_->setStatus(
      valid ? partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).limitReached
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Shell,
                                           ToolSelectionStage::SelectingInput)
                  : localizedPartDesignError(PartDesignToolKind::Shell,
                                             partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).failure),
      state == ToolLifecycle::PreviewInvalid);
  if (valid) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).bodyId});
  } else {
    viewport_->clearToolPreviewShape();
  }
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).linearManipulator)
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).limitReached)
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelShellTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptShellTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).lifecycle != ToolLifecycle::PreviewValid) return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previous = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  Body* body = mutableStandaloneDocument().findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).bodyId);
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* shell = dynamic_cast<ShellFeature*>(body->features()[index].get());
      if (!shell || shell->id() != *editingId) continue;
      shell->setRemovedFaces(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).faces);
      shell->setThicknessMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).thicknessMm);
      shell->setOutside(partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).outside);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ShellFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).sourceFeatureId, partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).faces,
        partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).thicknessMm, partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).outside,
        "Оболочка " + std::to_string(body->features().size())));
  }
  if (!mutableStandaloneDocument().rebuild()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
}

void MainWindow::createDraft() {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  resetTransientModelingUi();
  auto faces = viewport_->selectedBodyFaces();
  if (faces.size() > 1) faces.resize(1);
  Body* body = faces.empty() ? mutableStandaloneDocument().activeBody()
                             : mutableStandaloneDocument().findBody(faces.front().bodyId());
  if (!body || !body->activeFeature() || !body->resultShape()) return;
  if (!faces.empty() && faces.front().featureId() != body->activeFeature()->id())
    faces.clear();
  const auto sourceSnapshot =
      body->topologySnapshot(body->activeFeature()->id());
  if (!sourceSnapshot) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginDraft(
              activePartDocument(), *sourceSnapshot, faces, std::nullopt, std::nullopt,
              5.0, false),
          PartDesignToolKind::Draft))
    return;
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectedBodyFaces(faces);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Draft),
                                  QString::fromUtf8("Поверхность"),
                                  QString::fromUtf8("Угол"),
                                  QString::fromUtf8("°"));
  toolParametersPanel_->setParameterRange(-89.99, 89.99, 2);
  toolParametersPanel_->setParameterValue(5.0);
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateDraftToolPreview();
  if (faces.empty()) {
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingInput);
    viewport_->beginDraftFaceSelection();
    statusBar()->showMessage(
        QString::fromUtf8("1/3 Выберите поверхность в 3D-виде"));
  } else {
    partDesignCoordinator_.beginReselection(ToolSelectionStage::SelectingReference);
    viewport_->beginDraftAxisSelection();
    statusBar()->showMessage(
        QString::fromUtf8(
            "2/3 Выберите ось X, Y, Z или прилегающее прямое ребро"));
  }
}

void MainWindow::updateDraftToolPreview() {
  if (!partProjectActive()) return;
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).faces.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  toolParametersPanel_->setStatus(
      valid ? QString::fromUtf8("Предпросмотр построен · поверхность и ось выбраны")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Draft,
                                           ToolSelectionStage::SelectingInput)
                  : state == ToolLifecycle::SelectingReference
                        ? partDesignToolStepHint(
                              PartDesignToolKind::Draft,
                              ToolSelectionStage::SelectingReference)
                  : localizedPartDesignError(PartDesignToolKind::Draft,
                                             partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).failure),
      state == ToolLifecycle::PreviewInvalid);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).bodyId});
  } else {
    viewport_->clearToolPreviewShape();
  }
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).angularManipulator)
    viewport_->setAngularToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
}

void MainWindow::cancelDraftTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptDraftTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  const auto state =
      partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument());
  if (state.lifecycle != ToolLifecycle::PreviewValid ||
      (!state.rotationEdge && (!state.neutralPlane || !state.pullDirection)))
    return;
  Body* body = mutableStandaloneDocument().findBody(state.bodyId);
  if (!body || (state.sourceTopologySnapshot &&
                !body->matchesTopologySnapshot(*state.sourceTopologySnapshot)))
    return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previous = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* draft = dynamic_cast<DraftFeature*>(body->features()[index].get());
      if (!draft || draft->id() != *editingId) continue;
      draft->setDraftedFaces(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).faces);
      if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).neutralPlane)
        draft->setNeutralPlane(*partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).neutralPlane);
      if (partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).pullDirection)
        draft->setPullDirection(*partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).pullDirection);
      draft->setRotationEdge(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).rotationEdge);
      draft->setAngleDeg(std::abs(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).angleDeg));
      draft->setReversed(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).angleDeg < 0.0);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    const PlaneReference plane = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).neutralPlane.value_or(
        PlaneReference{NeutralPlaneType::GlobalXY});
    const AxisReference direction = partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).pullDirection.value_or(
        AxisReference{AxisReferenceType::GlobalZ, kInvalidSketchId,
                      sketch::kInvalidGeometryId});
    body->addFeature(std::make_unique<DraftFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).sourceFeatureId, partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).faces,
        plane, direction,
        std::abs(partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).angleDeg),
        partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).angleDeg < 0.0,
        "Уклон " + std::to_string(body->features().size()),
        partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, activePartDocument()).rotationEdge));
  }
  if (!mutableStandaloneDocument().rebuild()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
}

void MainWindow::createSketchExtrude(const ExtrusionSourcePick& pick) {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  const auto* region = std::get_if<SketchRegionPick>(&pick.source);
  if (!region || region->sketchId == kInvalidSketchId) return;
  const SketchId sketchId = region->sketchId;
  DocumentSketch* profile = mutableStandaloneDocument().findSketch(sketchId);
  if (!profile) return;
  Body* activeBody = mutableStandaloneDocument().activeBody();
  SketchProfileSelectionContext context;
  context.profile = *profile;
  std::optional<sketch::Sketch> profileOverride;
  const auto& pickedProfile = region->geometry;
  if (!pickedProfile.lines().empty() || !pickedProfile.circles().empty() ||
      !pickedProfile.arcs().empty() || !pickedProfile.beziers().empty()) {
    context.profile.geometry = pickedProfile;
    if (!isSupportedSingleSketchProfile(*profile))
      profileOverride = pickedProfile;
  }
  context.activeBodyId = activeBody ? activeBody->id() : kInvalidBodyId;
  context.activeFeatureId =
      activeBody && activeBody->activeFeature() ? activeBody->activeFeature()->id()
                                                : kInvalidFeatureId;
  context.activeBodyShape =
      activeBody ? activeBody->resultShape() : ShapeFeature::ShapePtr{};
  const auto capability = resolveSketchProfileExtrude(context);
  if (!capability) {
    statusBar()->showMessage(
        QString::fromUtf8("Прямое выдавливание: профиль не поддерживается"),
        4000);
    return;
  }
  resetTransientModelingUi();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginSketchExtrude(
              *profile, sketchId, capability->baseShape, 10.0,
              capability->operation, false, std::nullopt,
              std::move(profileOverride)),
          PartDesignToolKind::Extrude))
    return;
  partDesignCoordinator_.setExtrudeOperationFollowsDirection(
      capability->operationFollowsDirection);
  viewport_->clearLegacyExtrusionPreview();
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Профиль"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterRange(0.01, 100000.0, 2);
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm);
  viewport_->preserveWorldOriginOnNextResize();
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::createFaceExtrude(const FaceReference& face) {
  if (!partProjectActive()) return;
  // Native face extrusion shares the Extrude ribbon entry; picking a real
  // B-Rep body face (rather than a sketch contour) starts this face-source
  // session. Never interrupt a running revolve profile re-selection.
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, activePartDocument()).lifecycle != ToolLifecycle::Inactive) return;
  Body* body = mutableStandaloneDocument().findBody(face.bodyId());
  if (!body || !body->activeFeature() || !body->resultShape()) {
    statusBar()->showMessage(QString::fromUtf8("Сначала создайте тело."), 3000);
    return;
  }
  resetTransientModelingUi();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginExtrude(
              body->id(), body->activeFeature()->id(), body->resultShape(),
              face, 10.0, ExtrudeOperation::Join, false, std::nullopt,
              body->activeFeature()->topologyIndex()),
          PartDesignToolKind::Extrude))
    return;
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectedBodyFaces({face});
  viewport_->clearLegacyExtrusionPreview();
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Грань"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  const auto parameters = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).parameters;
  const double minimum = parameters.empty() ? 0.01 : parameters.front().minimum;
  const double maximum =
      parameters.empty() ? 100000.0 : parameters.front().maximum;
  toolParametersPanel_->setParameterRange(minimum, maximum, 2);
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm);
  toolParametersPanel_->configureOption(QString::fromUtf8("Вырезать"), false);
  viewport_->preserveWorldOriginOnNextResize();
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
}

void MainWindow::updateFaceExtrudeToolPreview() {
  if (!partProjectActive()) return;
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setSelectionCount(
      partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).sketchSource ||
              partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).face.bodyId() != kInvalidBodyId
          ? 1
          : 0);
  toolParametersPanel_->setAcceptEnabled(valid);
  if (valid) {
    toolParametersPanel_->setStatus(QString::fromUtf8("Предпросмотр построен"),
                                    false);
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).previewShape);
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation != ExtrudeOperation::NewBody)
      viewport_->setToolPreviewReplacedBodies(
          {partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).bodyId});
    viewport_->setToolCutPreviewShape(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).subtractivePreviewShape);
  } else {
    viewport_->clearToolPreviewShape();
    toolParametersPanel_->setStatus(
        state == ToolLifecycle::PreviewInvalid
            ? localizedFaceToolError(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).failure)
            : QString::fromUtf8("Выберите грань тела"),
        state == ToolLifecycle::PreviewInvalid);
  }
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).linearManipulator)
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(
        localizedFaceToolError(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).failure));
}

void MainWindow::acceptFaceExtrudeTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle != ToolLifecycle::PreviewValid) return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previous = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).sketchSource) {
    Body* targetBody = nullptr;
    if (partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation == ExtrudeOperation::NewBody) {
      targetBody = &mutableStandaloneDocument().addBody();
    } else {
      targetBody = mutableStandaloneDocument().activeBody();
      if (!targetBody) return;
    }
    auto feature = std::make_unique<ExtrudeFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).profileSketchId, partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm,
        "Extrude", partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation,
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).reversed);
    feature->setProfileOverride(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).profileOverride);
    targetBody->addFeature(std::move(feature));
    if (!mutableStandaloneDocument().recompute()) {
      const QString error = QString::fromStdString(activePartDocument().rebuildError());
      mutableStandaloneDocument() = previous;
      refreshBodyViewFromDocument();
      toolParametersPanel_->setStatus(error, true);
      return;
    }
    auto committedAfter = captureCommittedEndState();
    if (!pushModelTransition(std::move(previous), previousSelection,
                             std::move(committedAfter)))
      return;
    applyPartDesignUiEffect(commit.accept());
    rebuildFeatureTree();
    rebuildHistoryPanel();
    completeModelTransitionUi();
    statusBar()->showMessage(QString::fromUtf8("Создано выдавливание профиля"), 3000);
    return;
  }
  Body* body = mutableStandaloneDocument().findBody(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).bodyId);
  if (!body) return;
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* extrude =
          dynamic_cast<ExtrudeFeature*>(body->features()[index].get());
      if (!extrude || extrude->id() != *editingId) continue;
      extrude->setFaceReference(
          partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).face);
      extrude->setLengthMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm);
      extrude->setOperation(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation);
      extrude->setReversed(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).reversed);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ExtrudeFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).face, partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm, "Extrude",
        partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation, partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).reversed));
  }
  if (!mutableStandaloneDocument().recompute()) {
    const QString error = QString::fromStdString(activePartDocument().rebuildError());
    mutableStandaloneDocument() = previous;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Создано выдавливание грани"), 3000);
}

void MainWindow::cancelFaceExtrudeTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::editFaceExtrudeStep(Body* body, ExtrudeFeature* extrude,
                                     std::size_t extrudeIndex) {
  if (!partProjectActive()) return;
  if (!extrude->faceReference()) return;
  if (extrudeIndex == 0) {
    statusBar()->showMessage(
        QString::fromUtf8("Выдавливание грани не может быть первой операцией"),
        3000);
    return;
  }
  ShapeFeature::ShapePtr baseShape = body->features()[extrudeIndex - 1]->shape();
  if (!baseShape || baseShape->IsNull()) {
    statusBar()->showMessage(
        QString::fromUtf8("Не удалось восстановить исходное тело"), 3000);
    return;
  }
  const FeatureId sourceFeatureId = body->features()[extrudeIndex - 1]->id();
  const FaceReference face = *extrude->faceReference();
  viewport_->clearLegacyExtrusionPreview();
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginExtrude(
              body->id(), sourceFeatureId, baseShape, face,
              extrude->lengthMm(), extrude->operation(), extrude->reversed(),
              extrude->id(),
              body->features()[extrudeIndex - 1]->topologyIndex()),
          PartDesignToolKind::Extrude))
    return;
  viewport_->setSelectionFilter(SelectionFilter::Face);
  viewport_->setFaceMultiSelectionMode(false);
  viewport_->setSelectedBodyFaces({face});
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Extrude),
                                  QString::fromUtf8("Грань"),
                                  QString::fromUtf8("Длина"),
                                  QStringLiteral(" mm"));
  const auto parameters = partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).parameters;
  const double minimum = parameters.empty() ? 0.01 : parameters.front().minimum;
  const double maximum =
      parameters.empty() ? 100000.0 : parameters.front().maximum;
  toolParametersPanel_->setParameterRange(minimum, maximum, 2);
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lengthMm);
  toolParametersPanel_->configureOption(
      QString::fromUtf8("Вырезать"),
      partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation == ExtrudeOperation::Cut);
  viewport_->preserveWorldOriginOnNextResize();
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFaceExtrudeToolPreview();
  static_cast<void>(viewport_->focusToolParameterField(false));
  statusBar()->showMessage(
      QString::fromUtf8("Редактирование выдавливания грани"), 3000);
}

void MainWindow::updateChamferToolPreview() {
  if (!partProjectActive()) return;
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).edges.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  const auto maximum = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).maximumValidValueMm;
  const QString error = maximum
                            ? QString::fromUtf8(
                                  "Размер фаски слишком велик. Максимально "
                                  "допустимое значение: %1 мм.")
                                  .arg(*maximum, 0, 'f', 2)
                            : localizedPartDesignError(
                                  PartDesignToolKind::Chamfer,
                                  partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).failure);
  const QString limitStatus =
      maximum
          ? QString::fromUtf8("Достигнут предельный размер фаски: %1 мм")
                .arg(*maximum, 0, 'f', 2)
          : QString{};
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).distanceMm);
  toolParametersPanel_->setStatus(
      valid ? partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).limitReached
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Chamfer,
                                           ToolSelectionStage::SelectingInput)
                  : error,
      state == ToolLifecycle::PreviewInvalid);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).bodyId});
  } else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).linearManipulator)
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(error, 5000);
  else if (partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).limitReached)
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelChamferTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptChamferTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  const auto state =
      partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer);
  if (state.lifecycle != ToolLifecycle::PreviewValid) return;
  Body* body = mutableStandaloneDocument().findBody(state.bodyId);
  if (!body || (state.sourceTopologySnapshot &&
                !body->matchesTopologySnapshot(*state.sourceTopologySnapshot)))
    return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previousDocument = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* chamfer = dynamic_cast<ChamferFeature*>(body->features()[index].get());
      if (!chamfer || chamfer->id() != *editingId) continue;
      chamfer->setEdges(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).edges);
      chamfer->setDistanceMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).distanceMm);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<ChamferFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).edges, partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).distanceMm,
        "Фаска " + std::to_string(body->features().size())));
  }
  if (!mutableStandaloneDocument().rebuild()) {
    const QString error = localizedPartDesignError(
        PartDesignToolKind::Chamfer,
        {OperationFailureCode::Unknown, activePartDocument().rebuildError()});
    mutableStandaloneDocument() = previousDocument;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  const double distance = partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).distanceMm;
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(
      QString::fromUtf8("Фаска применена: %1 мм").arg(distance), 3000);
}

void MainWindow::updateFilletToolPreview() {
  if (!partProjectActive()) return;
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle;
  if (state == ToolLifecycle::Inactive) return;
  toolParametersPanel_->setSelectionCount(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).edges.size());
  const bool valid = state == ToolLifecycle::PreviewValid;
  toolParametersPanel_->setAcceptEnabled(valid);
  const auto maximum = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).maximumValidValueMm;
  const QString error = maximum
                            ? QString::fromUtf8(
                                  "Радиус скругления слишком велик. Максимально "
                                  "допустимое значение: %1 мм.")
                                  .arg(*maximum, 0, 'f', 2)
                            : localizedPartDesignError(
                                  PartDesignToolKind::Fillet,
                                  partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).failure);
  const QString limitStatus =
      maximum
          ? QString::fromUtf8("Достигнут предельный радиус: %1 мм")
                .arg(*maximum, 0, 'f', 2)
          : QString{};
  toolParametersPanel_->setParameterValue(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).radiusMm);
  toolParametersPanel_->setStatus(
      valid ? partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).limitReached
                  ? limitStatus
                  : QString::fromUtf8("Предпросмотр построен")
            : state == ToolLifecycle::SelectingInput
                  ? partDesignToolStepHint(PartDesignToolKind::Fillet,
                                           ToolSelectionStage::SelectingInput)
                  : error,
      state == ToolLifecycle::PreviewInvalid);
  if (partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).previewShape) {
    viewport_->setToolPreviewPresentation(
        ToolPreviewPresentation::ReplaceSource);
    viewport_->setToolPreviewShape(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).bodyId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).sourceFeatureId,
                                   partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).previewShape);
    viewport_->setToolPreviewReplacedBodies({partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).bodyId});
  } else
    viewport_->clearToolPreviewShape();
  if (const auto manipulator = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).linearManipulator)
    viewport_->setToolManipulator(*manipulator);
  else
    viewport_->clearToolManipulator();
  if (state == ToolLifecycle::PreviewInvalid)
    statusBar()->showMessage(error, 5000);
  else if (partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).limitReached)
    statusBar()->showMessage(limitStatus, 5000);
}

void MainWindow::cancelFilletTool() {
  if (!partProjectActive()) return;
  applyPartDesignUiEffect(
      partDesignCoordinator_.dispatchActiveAction(PartDesignAction::Cancel));
}

void MainWindow::acceptFilletTool() {
  if (!partProjectActive()) return;
  flushPreviewUpdate();
  const auto state = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet);
  if (state.lifecycle != ToolLifecycle::PreviewValid) return;
  Body* body = mutableStandaloneDocument().findBody(state.bodyId);
  if (!body || (state.sourceTopologySnapshot &&
                !body->matchesTopologySnapshot(*state.sourceTopologySnapshot)))
    return;
  auto commit = startPartDesignCommit();
  if (!commit) return;
  invalidatePreviewUpdates();
  Document previousDocument = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  if (const auto editingId = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).editingFeatureId) {
    for (std::size_t index = 0; index < body->features().size(); ++index) {
      auto* fillet = dynamic_cast<FilletFeature*>(body->features()[index].get());
      if (!fillet || fillet->id() != *editingId) continue;
      fillet->setEdges(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).edges);
      fillet->setRadiusMm(partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).radiusMm);
      body->markDirtyFrom(index);
      break;
    }
  } else {
    body->addFeature(std::make_unique<FilletFeature>(
        partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).edges, partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).radiusMm,
        "Скругление " + std::to_string(body->features().size())));
  }
  if (!mutableStandaloneDocument().rebuild()) {
    const QString error = localizedPartDesignError(
        PartDesignToolKind::Fillet,
        {OperationFailureCode::Unknown, activePartDocument().rebuildError()});
    mutableStandaloneDocument() = previousDocument;
    refreshBodyViewFromDocument();
    toolParametersPanel_->setStatus(error, true);
    return;
  }
  const double radius = partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).radiusMm;
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  applyPartDesignUiEffect(commit.accept());
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(
      QString::fromUtf8("Скругление применено: %1 мм").arg(radius), 3000);
}

void MainWindow::rebuildHistoryPanel() {
  if (!partProjectActive()) return;
  if (!historyLayout_) return;
  const int previousCount = static_cast<int>(historySteps_.size());
  const bool wasAtEnd = historyPosition_ >= previousCount;
  while (QLayoutItem* item = historyLayout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  historySteps_.clear();
  historySteps_ = buildPartDesignHistory(activePartDocument(), activePartDocument().activeBody());
  if (wasAtEnd) historyPosition_ = static_cast<int>(historySteps_.size());
  auto addStep = [this](const HistoryStep& step, int position) {
    auto* button = new QToolButton(historyContent_);
    configureHistoryButton(*button, step, position == historyPosition_);
    button->setProperty("bodyId", QVariant::fromValue<qulonglong>(step.bodyId));
    button->setProperty("featureId", QVariant::fromValue<qulonglong>(step.featureId));
    button->setProperty("sketchId", QVariant::fromValue<qulonglong>(step.sketchId));
    connect(button, &QToolButton::clicked, this, [this, step] {
      if (!partProjectActive()) return;
      if (step.sketchId != kInvalidSketchId) editSketchById(step.sketchId);
      else if (step.editable) editHistoryFeature(step.bodyId, step.featureId);
    });
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(button, &QToolButton::customContextMenuRequested, this,
            [this, button, step](const QPoint& point) {
      if (!partProjectActive()) return;
      // The history panel is rebuilt after removal. Keep the stack menu out
      // of the button's ownership and defer mutation until both QMenu::exec()
      // and the button's signal delivery have returned; otherwise rebuilding
      // destroys objects that Qt is still dispatching through.
      QMenu menu(this);
      QAction* edit = menu.addAction(QString::fromUtf8("Редактировать"));
      edit->setEnabled(step.editable);
      QAction* remove = menu.addAction(QString::fromUtf8("Удалить"));
      QAction* chosen = menu.exec(button->mapToGlobal(point));
      if (chosen == edit) {
        if (step.sketchId != kInvalidSketchId) editSketchById(step.sketchId);
        else editHistoryFeature(step.bodyId, step.featureId);
      } else if (chosen == remove) {
        const std::uint64_t documentGeneration =
            previewDocumentGeneration_;
        QTimer::singleShot(0, this,
                           [this, documentGeneration, step] {
          if (!partProjectActive()) return;
          if (documentGeneration != previewDocumentGeneration_) return;
          removeHistoryStep(step);
        });
      }
    });
    historyLayout_->addWidget(button);
  };
  for (std::size_t index = 0; index < historySteps_.size(); ++index)
    addStep(historySteps_[index], static_cast<int>(index + 1));
  if (historyTimeline_) {
    const QSignalBlocker blocker(historyTimeline_);
    const int lastPosition = static_cast<int>(historySteps_.size());
    historyTimeline_->setStepCount(lastPosition);
    historyPosition_ = std::clamp(historyPosition_, 0, lastPosition);
    historyTimeline_->setPosition(historyPosition_);
  }
  if (historyScroll_ && historyLayout_->count() > 0)
    historyScroll_->ensureWidgetVisible(
        historyLayout_->itemAt(historyLayout_->count() - 1)->widget());
}

bool MainWindow::ensureHistoryAtEnd() {
  if (!partProjectActive()) return false;
  if (isHistoryAtEnd()) return true;
  statusBar()->showMessage(
      QString::fromUtf8("Вернитесь к последнему шагу истории, чтобы добавить "
                        "новую операцию."),
      4000);
  return false;
}

bool MainWindow::isHistoryAtEnd() const {
  return historyPosition_ >= static_cast<int>(historySteps_.size());
}

void MainWindow::moveHistoryToEnd() {
  if (!partProjectActive()) return;
  historyPosition_ = static_cast<int>(historySteps_.size());
  if (historyTimeline_) historyTimeline_->setPosition(historyPosition_);
}

void MainWindow::removeHistoryStep(const HistoryStep& step) {
  if (!partProjectActive()) return;
  const FeatureRemovalPlan plan = step.sketchId != kInvalidSketchId
      ? activePartDocument().planSketchRemoval(step.sketchId)
      : activePartDocument().planFeatureRemoval(step.bodyId, step.featureId);
  if (!plan.applicable) {
    QMessageBox::warning(
        this, QString::fromUtf8("Ошибка удаления"),
        QString::fromStdString(plan.diagnostic.empty()
                                   ? "Не удалось построить план удаления"
                                   : plan.diagnostic));
    return;
  }
  if (plan.empty()) return;
  QStringList dependents;
  // historySteps_ contains only the active Body. Resolve the grouped plan
  // against Document so the confirmation names every cross-Body dependent
  // that will actually be removed.
  for (const auto& range : plan.bodyRanges) {
    const Body* body = mutableStandaloneDocument().findBody(range.bodyId);
    if (!body) continue;
    for (const FeatureId featureId : range.featureIds) {
      if (featureId == step.featureId) continue;
      const auto feature =
          std::find_if(body->features().begin(), body->features().end(),
                       [featureId](const auto& candidate) {
                         return candidate && candidate->id() == featureId;
                       });
      if (feature == body->features().end()) continue;
      dependents << QString::fromUtf8("• %1 / %2")
                        .arg(QString::fromStdString(body->name()),
                             QString::fromStdString((*feature)->name()));
    }
  }
  for (const SketchId sketchId : plan.sketchIds) {
    if (sketchId == step.sketchId) continue;
    const DocumentSketch* sketch = mutableStandaloneDocument().findSketch(sketchId);
    if (sketch)
      dependents << QString::fromUtf8("• Эскиз / %1")
                        .arg(QString::fromStdString(sketch->name));
  }
  QString message = QString::fromUtf8("Удалить «%1»?").arg(step.title);
  if (!dependents.isEmpty())
    message += QString::fromUtf8("\n\nОт этой операции зависят:\n") +
               dependents.join(QLatin1Char('\n')) +
               QString::fromUtf8("\n\nОни также будут удалены.");
  QMessageBox box(QMessageBox::Warning, QString::fromUtf8("Удаление операции"),
                  message, QMessageBox::Yes | QMessageBox::Cancel, this);
  box.button(QMessageBox::Yes)->setText(QString::fromUtf8("Удалить"));
  box.button(QMessageBox::Cancel)->setText(QString::fromUtf8("Отмена"));
  if (box.exec() != QMessageBox::Yes) return;

  const HistorySelectionState previousSelection = captureHistorySelection();
  Document previous = activePartDocument();
  const auto removedJoinOperands = removedJoinOperandIds(previous, plan);
  std::string error;
  const bool removed = mutableStandaloneDocument().applyRemovalPlan(plan, &error);
  if (removed)
    releaseUnconsumedJoinOperands(mutableStandaloneDocument(), removedJoinOperands);
  const bool recomputed = removed && mutableStandaloneDocument().recompute();
  const std::string rebuildError = recomputed ? std::string{} : activePartDocument().rebuildError();
  if (!recomputed) {
    mutableStandaloneDocument() = previous;
    QMessageBox::warning(this, QString::fromUtf8("Ошибка удаления"),
                         QString::fromStdString(
                             error.empty() ? rebuildError : error));
    return;
  }
  auto committedAfter = captureCommittedEndState();
  committedAfter.sketchViewIds.clear();
  committedAfter.presentation.sketchVisibilities.clear();
  for (std::size_t index = 0; index < sketchViews_.size(); ++index) {
    const SketchId id = sketchViews_[index].documentSketchId;
    if (!activePartDocument().findSketch(id)) continue;
    committedAfter.sketchViewIds.push_back(id);
    committedAfter.presentation.sketchVisibilities.push_back(
        previousSelection.presentation.sketchVisibilities.size() > index
            ? previousSelection.presentation.sketchVisibilities[index]
            : 1U);
  }
  if (!pushModelTransition(std::move(previous), previousSelection,
                           std::move(committedAfter)))
    return;
  resetTransientModelingUi();
  for (std::size_t index = sketchViews_.size(); index-- > 0;)
    if (!activePartDocument().findSketch(sketchViews_[index].documentSketchId)) {
      viewport_->removeSketch(index);
      sketchViews_.erase(sketchViews_.begin() +
                           static_cast<std::ptrdiff_t>(index));
    }
  viewport_->clearToolPreviewShape(); viewport_->clearToolManipulator();
  viewport_->setSelectedBodyEdges({}); viewport_->setSelectedBodyFaces({});
  toolParametersDock_->hide();
  revolveDock_->hide();
  mirrorDock_->hide();
  linearPatternDock_->hide();
  circularPatternDock_->hide();
  historyPosition_ = 1000000;
  refreshBodyViewFromDocument(); rebuildFeatureTree(); rebuildHistoryPanel();
  completeModelTransitionUi();
}

void MainWindow::removeBody(BodyId bodyId) {
  if (!partProjectActive()) return;
  if (!ensureHistoryAtEnd()) return;
  const Body* body = mutableStandaloneDocument().findBody(bodyId);
  if (!body) return;
  const FeatureRemovalPlan plan = activePartDocument().planBodyRemoval(bodyId);
  if (!plan.applicable) {
    QMessageBox::warning(
        this, QString::fromUtf8("Ошибка удаления"),
        QString::fromStdString(plan.diagnostic.empty()
                                   ? "Не удалось построить план удаления"
                                   : plan.diagnostic));
    return;
  }
  QStringList dependents;
  for (const auto& range : plan.bodyRanges) {
    const Body* dependentBody = mutableStandaloneDocument().findBody(range.bodyId);
    if (!dependentBody) continue;
    for (const FeatureId featureId : range.featureIds) {
      const auto feature = std::find_if(
          dependentBody->features().begin(), dependentBody->features().end(),
          [featureId](const auto& candidate) {
            return candidate && candidate->id() == featureId;
          });
      if (feature != dependentBody->features().end())
        dependents << QString::fromUtf8("• %1 / %2")
                          .arg(QString::fromStdString(dependentBody->name()),
                               QString::fromStdString((*feature)->name()));
    }
  }
  for (const SketchId sketchId : plan.sketchIds) {
    const DocumentSketch* sketch = mutableStandaloneDocument().findSketch(sketchId);
    if (sketch)
      dependents << QString::fromUtf8("• Эскиз / %1")
                        .arg(QString::fromStdString(sketch->name));
  }
  QString removalMessage = QString::fromUtf8("Удалить «%1»?")
                               .arg(QString::fromStdString(body->name()));
  if (!dependents.isEmpty())
    removalMessage += QString::fromUtf8("\n\nТакже будут удалены:\n") +
                      dependents.join(QLatin1Char('\n'));
  QMessageBox box(QMessageBox::Warning, QString::fromUtf8("Удаление Body"),
                  removalMessage, QMessageBox::Yes | QMessageBox::Cancel, this);
  box.button(QMessageBox::Yes)->setText(QString::fromUtf8("Удалить"));
  box.button(QMessageBox::Cancel)->setText(QString::fromUtf8("Отмена"));
  if (box.exec() != QMessageBox::Yes) return;

  const HistorySelectionState previousSelection = captureHistorySelection();
  Document previousDocument = activePartDocument();
  const auto removedJoinOperands =
      removedJoinOperandIds(previousDocument, plan);
  std::string error;
  const bool removed = mutableStandaloneDocument().applyRemovalPlan(plan, &error);
  if (removed)
    releaseUnconsumedJoinOperands(mutableStandaloneDocument(), removedJoinOperands);
  if (!removed || !mutableStandaloneDocument().recompute()) {
    const std::string rebuildError = activePartDocument().rebuildError();
    mutableStandaloneDocument() = previousDocument;
    QMessageBox::warning(
        this, QString::fromUtf8("Ошибка удаления"),
        QString::fromStdString(error.empty() ? rebuildError : error));
    return;
  }

  auto committedAfter = captureCommittedEndState();
  committedAfter.sketchViewIds.clear();
  committedAfter.presentation.sketchVisibilities.clear();
  for (std::size_t index = 0; index < sketchViews_.size(); ++index) {
    const SketchId id = sketchViews_[index].documentSketchId;
    if (!activePartDocument().findSketch(id)) continue;
    committedAfter.sketchViewIds.push_back(id);
    committedAfter.presentation.sketchVisibilities.push_back(
        previousSelection.presentation.sketchVisibilities.size() > index
            ? previousSelection.presentation.sketchVisibilities[index]
            : 1U);
  }
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  resetTransientModelingUi();
  for (std::size_t index = sketchViews_.size(); index-- > 0;)
    if (!activePartDocument().findSketch(sketchViews_[index].documentSketchId)) {
      viewport_->removeSketch(index);
      sketchViews_.erase(sketchViews_.begin() +
                           static_cast<std::ptrdiff_t>(index));
    }
  viewport_->setSelectedBodies({});
  viewport_->setSelectedBodyEdges({});
  viewport_->setSelectedBodyFaces({});
  historyPosition_ = 1000000;
  refreshBodyViewFromDocument();
  rebuildHistoryPanel();
  rebuildFeatureTree();
  completeModelTransitionUi();
  statusBar()->showMessage(QString::fromUtf8("Body удалён"), 2500);
}

void MainWindow::applyHistoryPosition(int position) {
  if (!partProjectActive()) return;
  const int lastPosition = static_cast<int>(historySteps_.size());
  historyPosition_ = std::clamp(position, 0, lastPosition);
  if (historyTimeline_) historyTimeline_->setPosition(historyPosition_);
  if (historyPosition_ == lastPosition) {
    refreshBodyViewFromDocument();
  } else {
    std::vector<BodyViewShape> shapes;
    for (const Body& body : activePartDocument().bodies()) {
      if (const Body* active = mutableStandaloneDocument().activeBody(); active && body.id() == active->id())
        continue;
      if (auto shape = body.lastValidResultShape()) {
        const ShapeFeature* feature = presentationOwner(body, shape);
        if (feature) {
          auto topology = feature->lastValidTopologyIndex();
          if (topology)
            shapes.push_back({body.id(), feature->id(), std::move(shape),
                              std::move(topology), feature->shapeRevision()});
        }
      }
    }
    if (historyPosition_ > 0) {
      const HistoryStep& step = historySteps_[historyPosition_ - 1];
      if (step.shape) {
        const auto* feature = findHistoryFeature(
            static_cast<const Document&>(activePartDocument()), step.bodyId,
            step.featureId);
        auto topology = feature ? feature->lastValidTopologyIndex() : nullptr;
        if (topology)
          shapes.push_back(
              {step.bodyId, step.featureId, step.shape, std::move(topology),
               feature ? feature->shapeRevision() : kInvalidShapeRevision});
      }
    }
    const bool hasHistoricalShape = !shapes.empty();
    viewport_->setBodyShapes(std::move(shapes));
    viewport_->setSolidVisible(hasHistoricalShape);
  }
  for (std::size_t index = 0; index < sketchViews_.size(); ++index) {
    const bool selectedSketch = historyPosition_ > 0 &&
        historySteps_[historyPosition_ - 1].sketchId ==
            sketchViews_[index].documentSketchId;
    viewport_->setSketchVisible(index, selectedSketch);
  }
  const auto buttons = historyContent_->findChildren<QToolButton*>("historyStep");
  for (int index = 0; index < buttons.size(); ++index)
    buttons[index]->setChecked(index + 1 == historyPosition_);
  rebuildFeatureTree();
  statusBar()->showMessage(
      historyPosition_ == lastPosition
          ? QString::fromUtf8("Активно последнее состояние модели")
          : QString::fromUtf8("История откачена до шага %1").arg(historyPosition_),
      2500);
}

bool MainWindow::configureSketchEditContext(bool autoProjectSupportFace) {
  if (!currentSketchFaceReference_) return true;
  const FaceReference reference = *currentSketchFaceReference_;
  const ShapeFeature* feature =
      activePartDocument().findFeature(reference.bodyId(), reference.featureId());
  const auto shape = feature ? feature->shape() : ShapeFeature::ShapePtr{};
  if (!shape) {
    QMessageBox::warning(
        this, QString::fromUtf8("Sketch on Face"),
        QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели."));
    return false;
  }
  const auto topology = feature->topologyIndex();
  const auto resolvedFace =
      topology ? topology->resolveFace(reference) : FaceResolution{};
  const auto resolved = resolvedFace
                            ? resolveFacePlacement(*resolvedFace.subshape)
                            : ResolvedFacePlacement{};
  if (!resolved.resolved) {
    QMessageBox::warning(
        this, QString::fromUtf8("Sketch on Face"),
        QString::fromUtf8("Не удалось восстановить выбранную грань после изменения модели."));
    return false;
  }
  if (!resolved.planar) {
    QMessageBox::information(
        this, QString::fromUtf8("Sketch on Face"),
        QString::fromUtf8("Создание эскиза на криволинейной поверхности пока не поддерживается."));
    return false;
  }
  currentSketchPlacement_ = resolved.placement;
  sketchCanvas_->setSketchEditContext(
      {kInvalidSketchId, currentSketchPlacement_, shape, topology, reference,
       autoProjectSupportFace});
  return true;
}

void MainWindow::configureSketchSceneReferences(SketchId excludedSketchId) {
  if (!partProjectActive()) return;
  const BodyId supportBodyId = currentSketchFaceReference_
                                   ? currentSketchFaceReference_->bodyId()
                                   : kInvalidBodyId;
  std::vector<ShapeFeature::ShapePtr> bodyShapes;
  bodyShapes.reserve(activePartDocument().bodies().size());
  for (const auto& body : activePartDocument().bodies()) {
    if (!body.visible()) continue;
    if (body.id() == supportBodyId) continue;
    if (auto shape = body.resultShape()) bodyShapes.push_back(std::move(shape));
  }

  std::vector<SketchSceneReference> sketches;
  sketches.reserve(activePartDocument().sketches().size());
  for (const auto& documentSketch : activePartDocument().sketches()) {
    if (documentSketch.id == excludedSketchId) continue;
    // Consumed construction sketches remain upstream of Move and other body
    // features.  Drawing all of them in blue made the pre-transform contours
    // look like a second, stale solid.  Match the 3D tree: show only sketches
    // that are still independently visible construction geometry.
    if (isSketchConsumedByPartDesign(activePartDocument(), documentSketch.id)) continue;
    sketches.push_back(
        {documentSketch.geometry, documentSketch.placement});
  }
  sketchCanvas_->setSceneReferences(currentSketchPlacement_, bodyShapes,
                                    std::move(sketches),
                                    activePartDocument().referenceImages());
}

void MainWindow::editSketchStep(std::size_t index) {
  if (!partProjectActive()) return;
  if (index >= sketchViews_.size()) return;
  const SketchId sketchId = sketchViews_[index].documentSketchId;
  if (!activePartDocument().findSketch(sketchId)) return;
  resetTransientModelingUi();
  const auto* modelSketch = mutableStandaloneDocument().findSketch(sketchId);
  if (!modelSketch) return;
  editingSketchIndex_ = index;
  currentSketchSupport_ = sketchPresentationLabel(*modelSketch);
  currentSketchFaceReference_.reset();
  currentSketchPlacement_ = modelSketch->placement;
  if (modelSketch->support.type == SketchSupportType::Face)
    currentSketchFaceReference_ = modelSketch->support.face;
  sketchCanvas_->clearSketchEditContext();
  sketchCanvas_->setInitialViewUp({});
  sketchCanvas_->setInitialViewDirection({});
  if (currentSketchFaceReference_ && !configureSketchEditContext()) {
    editingSketchIndex_.reset();
    return;
  }
  sketchCanvas_->loadSketch(modelSketch->geometry);
  configureSketchSceneReferences(sketchId);
  sketchCanvas_->setReferenceBody(activePartDocument().box(), currentSketchPlacement_,
                                  hasDisplayableModernSolid() ||
                                      hasHistoricalLegacyExtrusion());
  workspaceStack_->setCurrentWidget(sketchCanvas_);
  ribbonStack_->setCurrentWidget(sketchRibbon_);
  statusBar()->showMessage(
      QString::fromUtf8("Редактируется эскиз %1 • Завершите эскиз для перестроения")
          .arg(index + 1));
}

void MainWindow::editSketchById(SketchId sketchId) {
  if (!partProjectActive()) return;
  const auto found = std::find_if(
      sketchViews_.begin(), sketchViews_.end(), [sketchId](const auto& entry) {
        return entry.documentSketchId == sketchId;
      });
  if (found != sketchViews_.end())
    editSketchStep(static_cast<std::size_t>(
        std::distance(sketchViews_.begin(), found)));
}

void MainWindow::editHistoryFeature(BodyId bodyId, FeatureId featureId) {
  if (!partProjectActive()) return;
  Body* body = mutableStandaloneDocument().findBody(bodyId);
  if (!body) return;
  ShapeFeature* feature = findHistoryFeature(mutableStandaloneDocument(), bodyId, featureId);
  if (!feature) return;
  const auto* descriptor = featureUiDescriptor(feature->kind());
  if (!descriptor || !descriptor->editable ||
      descriptor->editorRoute == FeatureEditorRoute::None)
    return;
  // Switching directly from one editor to another first terminates the old
  // transaction. resetTransientModelingUi() continues clearing shared
  // viewport state after controller callbacks, so restore the old committed
  // state only after the complete teardown, then snapshot the new editor.
  if (editUiTransaction_) {
    const auto previousEdit = *editUiTransaction_;
    suppressEditCancelRestore_ = true;
    try {
      resetTransientModelingUi();
    } catch (...) {
      suppressEditCancelRestore_ = false;
      editUiTransaction_.reset();
      restoreHistorySelection(previousEdit.before);
      applyPresentationState(previousEdit.before.presentation);
      setWindowModified(previousEdit.modified);
      throw;
    }
    suppressEditCancelRestore_ = false;
    editUiTransaction_.reset();
    restoreHistorySelection(previousEdit.before);
    applyPresentationState(previousEdit.before.presentation);
    setWindowModified(previousEdit.modified);
  }
  const auto before = captureHistorySelection();
  const bool modified = isWindowModified();
  editUiTransaction_ = EditUiTransaction{
      before, bodyId, featureId, modified};
  suppressEditCancelRestore_ = true;
  try {
    resetTransientModelingUi();
  } catch (...) {
    suppressEditCancelRestore_ = false;
    editUiTransaction_.reset();
    restoreHistorySelection(before);
    applyPresentationState(before.presentation);
    setWindowModified(modified);
    throw;
  }
  suppressEditCancelRestore_ = false;

  const auto transactionMatchesRequest = [this, bodyId, featureId] {
    return editUiTransaction_ && editUiTransaction_->bodyId == bodyId &&
           editUiTransaction_->featureId == featureId;
  };
  const auto editorSuccessfullyActive = [this, featureId] {
    const auto state = partDesignCoordinator_.snapshot(
        partDesignCoordinator_.activeTool(), activePartDocument());
    return state.lifecycle != ToolLifecycle::Inactive &&
           state.editingFeatureId == std::optional<FeatureId>{featureId};
  };
  const auto abortFailedDispatch = [this, &transactionMatchesRequest] {
    if (!transactionMatchesRequest()) return;
    const EditUiTransaction transaction = *editUiTransaction_;
    suppressEditCancelRestore_ = true;
    try {
      resetTransientModelingUi();
    } catch (...) {
      suppressEditCancelRestore_ = false;
      editUiTransaction_.reset();
      throw;
    }
    suppressEditCancelRestore_ = false;
    editUiTransaction_.reset();
    restoreHistorySelection(transaction.before);
    applyPresentationState(transaction.before.presentation);
    setWindowModified(transaction.modified);
  };

  try {
    switch (descriptor->editorRoute) {
      case FeatureEditorRoute::Extrude:
        editExtrusionStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::Pocket:
        editPocketStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::Fillet:
        editFilletStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::Chamfer:
        editChamferStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::JoinBodies:
        editJoinBodiesStep(bodyId, featureId);
        break;
      case FeatureEditorRoute::Revolve:
      case FeatureEditorRoute::Mirror:
      case FeatureEditorRoute::Move:
      case FeatureEditorRoute::LinearPattern:
      case FeatureEditorRoute::CircularPattern:
      case FeatureEditorRoute::Shell:
      case FeatureEditorRoute::Draft:
        editPatternFeature(featureId, descriptor->editorRoute);
        break;
      case FeatureEditorRoute::None:
        break;
    }
  } catch (...) {
    abortFailedDispatch();
    throw;
  }

  // Modal editors consume the transaction before returning. Persistent
  // editors retain it only when their exact feature/session is active. Every
  // unsupported or invalid-source early return is therefore an atomic no-op,
  // including selection/history/modified state and transient presentation.
  if (transactionMatchesRequest() && !editorSuccessfullyActive())
    abortFailedDispatch();
}

void MainWindow::editExtrusionStep(BodyId bodyId, FeatureId featureId) {
  if (!partProjectActive()) return;
  Body* body = mutableStandaloneDocument().findBody(bodyId);
  ExtrudeFeature* extrude = nullptr;
  std::size_t extrudeIndex = 0;
  if (body)
    for (std::size_t index = 0; index < body->features().size(); ++index)
      if (auto* candidate =
              dynamic_cast<ExtrudeFeature*>(body->features()[index].get());
          candidate && candidate->id() == featureId) {
        extrude = candidate;
        extrudeIndex = index;
        break;
      }
  if (!extrude) return;
  if (extrude->isFaceSource()) {
    editFaceExtrudeStep(body, extrude, extrudeIndex);
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle(QString::fromUtf8("Изменить выдавливание"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* form = new QFormLayout;
  auto* distance = new QDoubleSpinBox(&dialog);
  distance->setRange(-100000.0, 100000.0);
  distance->setDecimals(2);
  distance->setSuffix(QStringLiteral(" mm"));
  distance->setValue(extrude->reversed() ? -extrude->lengthMm()
                                           : extrude->lengthMm());
  auto* operation = new QComboBox(&dialog);
  operation->addItems({QString::fromUtf8("Новое тело"),
                       QString::fromUtf8("Объединить"),
                       QString::fromUtf8("Вырезать")});
  operation->setCurrentIndex(static_cast<int>(extrude->operation()));
  // Moving an existing feature between Bodies is intentionally outside 2.0.
  if (extrude->operation() == ExtrudeOperation::NewBody)
    operation->setEnabled(false);
  else
    operation->setItemData(0, 0, Qt::UserRole - 1);
  form->addRow(QString::fromUtf8("Расстояние:"), distance);
  form->addRow(QString::fromUtf8("Операция:"), operation);
  layout->addLayout(form);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                        QDialogButtonBox::Cancel, &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) {
    restoreCancelledEditUiTransaction();
    return;
  }
  const double height = distance->value();
  Document previousDocument = activePartDocument();
  const HistorySelectionState previousSelection = captureHistorySelection();
  extrude->setLengthMm(std::abs(height));
  extrude->setReversed(height < 0.0);
  if (extrude->operation() != ExtrudeOperation::NewBody)
    extrude->setOperation(static_cast<ExtrudeOperation>(operation->currentIndex()));
  body->markDirtyFrom(extrudeIndex);
  if (!mutableStandaloneDocument().rebuild()) {
    const QString error = QString::fromStdString(extrude->error());
    mutableStandaloneDocument() = previousDocument;
    refreshBodyViewFromDocument();
    QMessageBox::warning(this, QString::fromUtf8("Ошибка выдавливания"), error);
    return;
  }
  auto committedAfter = captureCommittedEndState();
  if (!pushModelTransition(std::move(previousDocument), previousSelection,
                           std::move(committedAfter)))
    return;
  refreshBodyViewFromDocument();
  rebuildFeatureTree();
  rebuildHistoryPanel();
  completeModelTransitionUi();
  statusBar()->showMessage(
      QString::fromUtf8("Выдавливание изменено: %1 мм").arg(height), 3000);
}

void MainWindow::editPocketStep(BodyId bodyId, FeatureId featureId) {
  if (!partProjectActive()) return;
  Body* body = mutableStandaloneDocument().findBody(bodyId);
  PocketFeature* pocket = nullptr;
  if (body)
    for (const auto& candidate : body->features())
      if (candidate->id() == featureId) {
        pocket = dynamic_cast<PocketFeature*>(candidate.get()); break;
      }
  if (!pocket) return;
  bool accepted = false;
  const double depth = QInputDialog::getDouble(
      this, QString::fromUtf8("Изменить карман"),
      QString::fromUtf8("Глубина, мм:"), pocket->depthMm(),
      0.01, 100000.0, 2, &accepted);
  if (!accepted) {
    restoreCancelledEditUiTransaction();
    return;
  }
  if (!acceptPocketTransition(pocket->profileSketchId(), depth, pocket->id()))
    restoreCancelledEditUiTransaction();
}

void MainWindow::editFilletStep(BodyId bodyId, FeatureId featureId) {
  if (!partProjectActive()) return;
  Body* body = mutableStandaloneDocument().findBody(bodyId);
  FilletFeature* fillet = nullptr;
  if (body)
    for (const auto& candidate : body->features())
      if (candidate->id() == featureId) {
        fillet = dynamic_cast<FilletFeature*>(candidate.get()); break;
      }
  if (!fillet) return;
  const auto& features = body->features();
  if (features.size() < 2) return;
  ShapeFeature* upstreamFeature = nullptr;
  for (std::size_t index = 1; index < features.size(); ++index)
    if (features[index].get() == fillet) {
      upstreamFeature = features[index - 1].get();
      break;
    }
  if (!upstreamFeature) return;
  const auto sourceSnapshot = body->topologySnapshot(upstreamFeature->id());
  if (!sourceSnapshot) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginFillet(
              *sourceSnapshot, fillet->edges(), fillet->radiusMm(),
              fillet->id()),
          PartDesignToolKind::Fillet))
    return;
  viewport_->setEdgeMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Edge);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Fillet),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Радиус"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterValue(fillet->radiusMm());
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateFilletToolPreview();
  viewport_->setSelectedBodyEdges(fillet->edges());
}

void MainWindow::editChamferStep(BodyId bodyId, FeatureId featureId) {
  if (!partProjectActive()) return;
  Body* body = mutableStandaloneDocument().findBody(bodyId);
  ChamferFeature* chamfer = nullptr;
  if (body)
    for (const auto& candidate : body->features())
      if (candidate->id() == featureId) {
        chamfer = dynamic_cast<ChamferFeature*>(candidate.get()); break;
      }
  if (!chamfer) return;
  const auto& features = body->features();
  if (features.size() < 2) return;
  ShapeFeature* upstreamFeature = nullptr;
  for (std::size_t index = 1; index < features.size(); ++index)
    if (features[index].get() == chamfer) {
      upstreamFeature = features[index - 1].get();
      break;
    }
  if (!upstreamFeature) return;
  const auto sourceSnapshot = body->topologySnapshot(upstreamFeature->id());
  if (!sourceSnapshot) return;
  if (!applyPartDesignBeginResult(
          partDesignCoordinator_.beginChamfer(
              *sourceSnapshot, chamfer->edges(), chamfer->distanceMm(),
              chamfer->id()),
          PartDesignToolKind::Chamfer))
    return;
  viewport_->setEdgeMultiSelectionMode(true);
  viewport_->setSelectionFilter(SelectionFilter::Edge);
  toolParametersPanel_->configure(*partDesignToolHelp(PartDesignToolKind::Chamfer),
                                  QString::fromUtf8("Рёбра"),
                                  QString::fromUtf8("Размер"),
                                  QStringLiteral(" mm"));
  toolParametersPanel_->setParameterValue(chamfer->distanceMm());
  toolParametersDock_->show();
  toolParametersDock_->raise();
  updateChamferToolPreview();
  viewport_->setSelectedBodyEdges(chamfer->edges());
}

void MainWindow::rebuildFeatureTree() {
  if (!partModelingContextActive()) return;
  const QSignalBlocker blocker(featureTree_);
  featureTree_->clear();
  const QString treeRootName =
      componentDefinitionEditContext_
          ? componentDefinitionEditContext_->displayName
          : QString::fromUtf8("Корпус детали");
  auto* project = new QTreeWidgetItem(featureTree_, {treeRootName});
  auto* origin = new QTreeWidgetItem(project, {QString::fromUtf8("Начало координат")});
  origin->setData(0, Qt::UserRole, 1);
  origin->setFlags(origin->flags() | Qt::ItemIsUserCheckable);
  origin->setCheckState(
      0, viewport_->originVisible() ? Qt::Checked : Qt::Unchecked);
  for (int plane = 0; plane < 3; ++plane) {
    const QString name = plane == 0 ? "XY" : plane == 1 ? "XZ" : "YZ";
    auto* planeItem = new QTreeWidgetItem(
        origin, {QString::fromUtf8("Плоскость ") + name});
    planeItem->setData(0, Qt::UserRole, 10 + plane);
    planeItem->setFlags(planeItem->flags() | Qt::ItemIsUserCheckable);
    planeItem->setCheckState(
        0, viewport_->basePlaneVisible(plane) ? Qt::Checked : Qt::Unchecked);
  }

  auto* sketches = new QTreeWidgetItem(project, {QString::fromUtf8("Эскизы")});
  const std::size_t activeSketchCount = std::min<std::size_t>(
      static_cast<std::size_t>(std::max(historyPosition_, 0)),
      sketchViews_.size());
  // During a just-committed direct operation rebuildFeatureTree() can run
  // before rebuildHistoryPanel() has appended the new history step. Treat the
  // previous end marker as "at end" so the newly created Body is not hidden.
  const bool historyAtEnd =
      historyPosition_ >= static_cast<int>(historySteps_.size());
  const bool activeSolid = historyAtEnd
      ? hasCommittedModernSolid() || hasHistoricalLegacyExtrusion()
      : historyPosition_ > 0 &&
            historyPosition_ <= static_cast<int>(historySteps_.size()) &&
            historySteps_[historyPosition_ - 1].shape;
  if (activeSketchCount == 0) {
    new QTreeWidgetItem(sketches, {QString::fromUtf8("Эскизов нет")});
  } else {
    for (std::size_t index = 0; index < activeSketchCount; ++index) {
      auto* sketchItem = new QTreeWidgetItem(
          sketches, {QString::fromUtf8("⌞  Эскиз %1").arg(index + 1)});
      sketchItem->setData(0, Qt::UserRole, 20 + static_cast<int>(index));
      sketchItem->setFlags(sketchItem->flags() | Qt::ItemIsUserCheckable);
      const SketchId sketchId = sketchViews_[index].documentSketchId;
      bool visible = true;
      if (historyAtEnd) {
        // Modern consumption comes only from Document dependencies: direct
        // Join/Cut and downstream features may consume several profiles. A
        // historical v1 extrusion has no Body graph, so its source SketchId
        // is intentionally used only by this compatibility presentation.
        const bool consumedByHistoricalLegacyExtrusion =
            hasHistoricalLegacyExtrusion() &&
            historicalLegacyExtrusionSourceSketchId_ == sketchId;
        visible = !consumedByHistoricalLegacyExtrusion &&
                  !isSketchConsumedByPartDesign(activePartDocument(), sketchId);
      } else {
        // Keep the tree checkbox synchronized with applyHistoryPosition():
        // when inspecting an earlier history step, only the selected sketch
        // step is shown; feature steps show no construction sketch by default.
        visible =
            historyPosition_ > 0 &&
            historyPosition_ <= static_cast<int>(historySteps_.size()) &&
            historySteps_[historyPosition_ - 1].sketchId == sketchId;
      }
      sketchItem->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      viewport_->setSketchVisible(index, visible);
    }
  }
  auto* images =
      new QTreeWidgetItem(project, {QString::fromUtf8("Изображения")});
  QTreeWidgetItem* selectedImageItem = nullptr;
  if (activePartDocument().referenceImages().empty()) {
    new QTreeWidgetItem(images, {QString::fromUtf8("Изображений нет")});
  } else {
    for (const auto& image : activePartDocument().referenceImages()) {
      auto* imageItem = new QTreeWidgetItem(
          images, {QString::fromUtf8("▧  ") + QString::fromStdString(image.name)});
      imageItem->setData(0, Qt::UserRole, 4);
      imageItem->setData(0, Qt::UserRole + 3,
                         QVariant::fromValue<qulonglong>(image.id));
      imageItem->setToolTip(
          0, QString::fromStdString(image.sourcePath) +
                 QString::fromUtf8("\nПлоскость: ") +
                 QString::fromStdString(image.supportName));
      imageItem->setFlags(imageItem->flags() | Qt::ItemIsUserCheckable);
      imageItem->setCheckState(0,
                               image.visible ? Qt::Checked : Qt::Unchecked);
      if (image.id == selectedReferenceImageId_)
        selectedImageItem = imageItem;
    }
  }
  auto* models = new QTreeWidgetItem(project, {QString::fromUtf8("Модели")});
  if (!activeSolid || activePartDocument().bodies().empty()) {
    new QTreeWidgetItem(models, {QString::fromUtf8("Твёрдых тел нет")});
  } else {
    for (std::size_t bodyIndex = 0; bodyIndex < activePartDocument().bodies().size(); ++bodyIndex) {
      const Body& body = activePartDocument().bodies()[bodyIndex];
      const QString bodyName = body.name().empty()
                                   ? QString::fromUtf8("Body%1").arg(
                                         bodyIndex + 1, 3, 10,
                                         QLatin1Char('0'))
                                   : QString::fromStdString(body.name());
      auto* bodyItem = new QTreeWidgetItem(
          models, {QString::fromUtf8("▣  ") + bodyName});
      bodyItem->setData(0, Qt::UserRole, 3);
      bodyItem->setData(0, Qt::UserRole + 2,
                        QVariant::fromValue<qulonglong>(body.id()));
      bodyItem->setFlags(bodyItem->flags() | Qt::ItemIsUserCheckable);
      bodyItem->setCheckState(0,
                              body.visible() ? Qt::Checked : Qt::Unchecked);
    }
  }
  featureTree_->expandAll();
  if (selectedImageItem) {
    featureTree_->setCurrentItem(selectedImageItem);
    showSelectedReferenceImageManipulator();
  } else if (selectedReferenceImageId_ != kInvalidReferenceImageId) {
    selectedReferenceImageId_ = kInvalidReferenceImageId;
    if (viewport_->referenceImageManipulatorActive())
      viewport_->clearToolManipulator();
  }
}

void MainWindow::exportPdf() {
  const QString fileName = QFileDialog::getSaveFileName(
      this, QString::fromUtf8("Экспорт чертежа в PDF"), "drawing-a4.pdf",
      QString::fromUtf8("PDF (*.pdf)"));
  if (fileName.isEmpty()) return;

  QPrinter printer(QPrinter::HighResolution);
  printer.setOutputFormat(QPrinter::PdfFormat);
  printer.setOutputFileName(fileName);
  printer.setPageSize(QPageSize(QPageSize::A4));
  printer.setPageOrientation(QPageLayout::Portrait);
  printer.setFullPage(true);

  QPainter painter;
  if (!painter.begin(&printer)) {
    QMessageBox::critical(this, QString::fromUtf8("Ошибка"),
                          QString::fromUtf8("Не удалось создать PDF."));
    return;
  }
  drawing::EskdRenderer::renderA4(painter, painter.viewport(),
                                  sketchCanvas_->sketch(), {},
                                  drawingSheet_->sourceShape());
  painter.end();
  statusBar()->showMessage(QString::fromUtf8("PDF сохранён: ") + fileName,
                           4000);
}

void MainWindow::printDrawing() {
  QPrinter printer(QPrinter::HighResolution);
  printer.setPageSize(QPageSize(QPageSize::A4));
  printer.setPageOrientation(QPageLayout::Portrait);
  printer.setFullPage(true);
  QPrintDialog dialog(&printer, this);
  dialog.setWindowTitle(QString::fromUtf8("Печать чертежа A4"));
  if (dialog.exec() != QDialog::Accepted) return;

  QPainter painter(&printer);
  drawing::EskdRenderer::renderA4(painter, painter.viewport(),
                                  sketchCanvas_->sketch(), {},
                                  drawingSheet_->sourceShape());
}

}  // namespace solidar
