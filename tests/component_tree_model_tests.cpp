#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QCoreApplication>

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "presentation/ComponentPresentationBuilder.h"
#include "presentation/ComponentScenePublicationCoordinator.h"
#include "ui/ComponentTreeModel.h"

namespace solidar {

class ComponentDrawingValidationTestAccess final {
 public:
  static void append(ComponentDefinition& definition,
                     ComponentDrawing drawing) {
    definition.drawings_.push_back(std::move(drawing));
  }
};

}  // namespace solidar

namespace {

using namespace solidar;

constexpr ProductPresentationEpoch kEpoch{7101};
constexpr AssemblyMeshProfile kNormal{AssemblyMeshQuality::Normal};

class AcceptSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    return ProductDocumentSuccessorStatus::Prepared;
  }
};

class RejectSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    return ProductDocumentSuccessorStatus::Rejected;
  }
};

static_assert(std::variant_size_v<ComponentTreeAddressValue> == 10);
static_assert(std::is_same_v<
              ComponentTreeModel::AddressView,
              std::optional<std::reference_wrapper<
                  const ComponentTreeNodeAddress>>>);
static_assert(noexcept(std::declval<const ComponentTreeModel&>()
                           .addressForIndex(
                               std::declval<const QModelIndex&>())));

std::shared_ptr<const TopoDS_Shape> box(double extent) {
  return std::make_shared<const TopoDS_Shape>(
      BRepPrimAPI_MakeBox(extent, extent + 1.0, extent + 2.0).Shape());
}

std::unique_ptr<Document> document(std::string prefix,
                                   bool firstBodyVisible = true,
                                   bool firstImageVisible = true) {
  auto value = std::make_unique<Document>();
  value->addSketch(SketchId{7}, prefix + " sketch first");
  value->addSketch(SketchId{2}, prefix + " sketch second");
  ReferenceImage first;
  first.id = ReferenceImageId{7};
  first.name = prefix + " image first";
  first.sourcePath = prefix + "-first.png";
  first.supportName = "XY";
  first.pixelWidth = 16;
  first.pixelHeight = 16;
  first.visible = firstImageVisible;
  value->addReferenceImage(std::move(first));
  ReferenceImage second;
  second.id = ReferenceImageId{1};
  second.name = prefix + " image second";
  second.sourcePath = prefix + "-second.png";
  second.supportName = "XZ";
  second.pixelWidth = 8;
  second.pixelHeight = 8;
  value->addReferenceImage(std::move(second));
  Body& firstBody = value->addBody(BodyId{7}, prefix + " body first");
  firstBody.setVisible(firstBodyVisible);
  firstBody.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{70}, box(2.0), prefix + " feature first"));
  Body& secondBody = value->addBody(BodyId{3}, prefix + " body second");
  secondBody.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{30}, box(1.0), prefix + " feature second"));
  CHECK(value->recompute());
  return value;
}

struct Fixture final {
  std::unique_ptr<ProductDocument> product;
};

std::vector<ComponentDrawing> drawings(std::string prefix,
                                       bool includeSecond = false) {
  std::vector<ComponentDrawing> result;
  result.emplace_back(
      DrawingId{7}, prefix + " drawing first", ComponentDrawingPaper::A4,
      ComponentDrawingOrientation::Portrait, 1.0,
      std::vector<BodyId>{BodyId{7}});
  if (includeSecond)
    result.emplace_back(
        DrawingId{2}, prefix + " drawing second", ComponentDrawingPaper::A4,
        ComponentDrawingOrientation::Landscape, 2.5,
        std::vector<BodyId>{BodyId{3}, BodyId{7}});
  return result;
}

Fixture graphProduct() {
  ComponentDefinition root(DefinitionId{300}, document("root", false, false),
                           drawings("root", true));
  ComponentDefinition middle(DefinitionId{200}, document("middle"),
                             drawings("middle"));
  ComponentDefinition leaf(DefinitionId{100}, document("leaf"),
                           drawings("leaf"));
  ComponentDefinition unused(DefinitionId{400}, document("unused"),
                             drawings("unused"));
  CHECK(middle.insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{100},
            RigidTransform3d::identity(),
            {.visible = true, .suppressed = false, .grounded = true},
            "Лист")) == ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{20}, DefinitionId{200},
            RigidTransform3d::identity(),
            {.visible = false, .suppressed = true, .grounded = false},
            "Правая ветвь")) == ComponentOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{200},
            RigidTransform3d::identity(), {}, "Левая ветвь")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(middle));
  definitions.push_back(std::move(unused));
  definitions.push_back(std::move(root));
  definitions.push_back(std::move(leaf));
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{300});
  CHECK(created);
  return {std::move(created.product)};
}

QModelIndex section(const ComponentTreeModel& model,
                    const QModelIndex& instance,
                    ComponentTreeGroupKind kind) {
  for (int row = 0; row < model.rowCount(instance); ++row) {
    const QModelIndex candidate = model.index(row, 0, instance);
    const auto address = model.addressForIndex(candidate);
    if (!address) continue;
    const auto* group =
        std::get_if<ComponentTreeGroupAddress>(&address->get().value());
    if (group && group->group == kind) return candidate;
  }
  return {};
}

void verifyRoundTripRecursively(const ComponentTreeModel& model,
                                const QModelIndex& parent = {}) {
  for (int row = 0; row < model.rowCount(parent); ++row) {
    const QModelIndex index = model.index(row, 0, parent);
    CHECK(index.isValid());
    const auto address = model.addressForIndex(index);
    CHECK(address);
    CHECK(model.indexForAddress(address->get()) == index);
    verifyRoundTripRecursively(model, index);
  }
}

void verifyHierarchyOrderingAddressesAndRoles() {
  auto fixture = graphProduct();
  ComponentScenePublicationCoordinator coordinator;
  CHECK(coordinator.rebuildAndPublish(*fixture.product, kEpoch, kNormal));

  const auto resourceFailure = ComponentTreeModel::createTyped(
      *fixture.product, coordinator.active(), nullptr,
      ComponentTreeModelFailurePoint::ResourceFailure);
  CHECK(resourceFailure.status ==
        ComponentTreeModelCreateStatus::ResourceFailure);
  CHECK(!resourceFailure.model);
  const auto limitFailure = ComponentTreeModel::createTyped(
      *fixture.product, coordinator.active(), nullptr,
      ComponentTreeModelFailurePoint::None,
      ComponentTreeBuildLimits{1});
  CHECK(limitFailure.status ==
        ComponentTreeModelCreateStatus::NodeLimitExceeded);
  CHECK(!limitFailure.model);

  auto model = ComponentTreeModel::create(
      *fixture.product, coordinator.active());
  CHECK(model);
  CHECK(model->columnCount() == 1);
  CHECK(model->rowCount() == 2);
  const QModelIndex root = model->index(0, 0);
  CHECK(root.isValid());
  const auto rootAddress = model->addressForIndex(root);
  CHECK(rootAddress);
  const auto* typedRoot =
      std::get_if<ComponentTreeRootAddress>(&rootAddress->get().value());
  CHECK(typedRoot);
  CHECK(typedRoot->instance.rootInstance());
  CHECK(typedRoot->instance.definitionId == DefinitionId{300});
  CHECK(model->rowCount(root) == 7);
  CHECK(model->data(root).toString() == QString::fromUtf8("Новый проект"));
  CHECK(model->data(root, Qt::CheckStateRole).toInt() == Qt::Checked);
  model->setRootDisplayName(QString::fromUtf8("Редуктор"));
  CHECK(model->data(root).toString() == QString::fromUtf8("Редуктор"));
  model->setRootVisible(false);
  CHECK(model->data(root, Qt::CheckStateRole).toInt() == Qt::Unchecked);
  model->setRootVisible(true);
  CHECK(model->data(model->index(0, 0, root)).toString() ==
        QString::fromUtf8("Начало координат"));
  CHECK(model->data(model->index(1, 0, root)).toString() ==
        QString::fromUtf8("Тела"));
  CHECK(model->data(model->index(2, 0, root)).toString() ==
        QString::fromUtf8("Эскизы"));
  CHECK(model->data(model->index(3, 0, root)).toString() ==
        QString::fromUtf8("Изображения"));
  CHECK(model->data(model->index(4, 0, root)).toString() ==
        QString::fromUtf8("Чертежи"));

  const QModelIndex origin = section(
      *model, root, ComponentTreeGroupKind::Origin);
  CHECK(model->rowCount(origin) == 3);
  CHECK(model->data(model->index(0, 0, origin)).toString() ==
        QString::fromUtf8("Плоскость XY"));
  CHECK(model->data(model->index(1, 0, origin)).toString() ==
        QString::fromUtf8("Плоскость XZ"));
  CHECK(model->data(model->index(2, 0, origin)).toString() ==
        QString::fromUtf8("Плоскость YZ"));

  const QModelIndex bodies = section(
      *model, root, ComponentTreeGroupKind::Bodies);
  const QModelIndex sketches = section(
      *model, root, ComponentTreeGroupKind::Sketches);
  const QModelIndex images = section(
      *model, root, ComponentTreeGroupKind::Images);
  const QModelIndex drawingSection = section(
      *model, root, ComponentTreeGroupKind::Drawings);
  CHECK(model->rowCount(bodies) == 2);
  CHECK(model->data(model->index(0, 0, bodies)).toString() ==
        QString::fromUtf8("root body first"));
  CHECK(model->data(model->index(1, 0, bodies)).toString() ==
        QString::fromUtf8("root body second"));
  CHECK(!model->data(model->index(0, 0, bodies),
                     kComponentTreeBodyVisibleRole).toBool());
  CHECK(model->data(bodies, Qt::CheckStateRole).toInt() ==
        Qt::PartiallyChecked);
  CHECK(model->flags(model->index(0, 0, bodies)) &
        Qt::ItemIsUserCheckable);
  CHECK(model->rowCount(sketches) == 2);
  CHECK(model->data(model->index(0, 0, sketches)).toString() ==
        QString::fromUtf8("root sketch first"));
  CHECK(model->data(model->index(1, 0, sketches)).toString() ==
        QString::fromUtf8("root sketch second"));
  CHECK(model->rowCount(images) == 2);
  CHECK(model->data(model->index(0, 0, images)).toString() ==
        QString::fromUtf8("root image first"));
  CHECK(!model->data(model->index(0, 0, images),
                     kComponentTreeImageVisibleRole).toBool());
  CHECK(model->data(images, Qt::CheckStateRole).toInt() ==
        Qt::PartiallyChecked);
  CHECK(model->rowCount(drawingSection) == 2);
  const QModelIndex firstDrawing = model->index(0, 0, drawingSection);
  const QModelIndex secondDrawing = model->index(1, 0, drawingSection);
  CHECK(model->data(firstDrawing).toString() ==
        QString::fromUtf8("root drawing first"));
  CHECK(model->data(secondDrawing).toString() ==
        QString::fromUtf8("root drawing second"));
  CHECK(model->data(firstDrawing, Qt::ToolTipRole).toString().contains(
      QString::fromUtf8("книжная")));
  CHECK(model->data(secondDrawing, Qt::ToolTipRole).toString().contains(
      QString::fromUtf8("2.5")));
  const auto* rootProjection =
      coordinator.active().snapshotIfCurrent()->treeProjection().findDefinition(
          DefinitionId{300});
  CHECK(rootProjection);
  CHECK(rootProjection->drawings.size() == 2);
  CHECK(rootProjection->drawings[0].id == DrawingId{7});
  CHECK(rootProjection->drawings[1].id == DrawingId{2});
  CHECK(rootProjection->drawings[1].paper == ComponentDrawingPaper::A4);
  CHECK(rootProjection->drawings[1].orientation ==
        ComponentDrawingOrientation::Landscape);
  CHECK(rootProjection->drawings[1].scale == 2.5);
  CHECK(rootProjection->drawings[1].sourceBodyIds ==
        std::vector<BodyId>({BodyId{3}, BodyId{7}}));

  // Same local numeric ID remains type-safe across body/sketch/image nodes.
  const auto bodyAddress = model->addressForIndex(model->index(0, 0, bodies));
  const auto sketchAddress =
      model->addressForIndex(model->index(0, 0, sketches));
  const auto imageAddress = model->addressForIndex(model->index(0, 0, images));
  const auto drawingAddress = model->addressForIndex(firstDrawing);
  CHECK(bodyAddress && sketchAddress && imageAddress && drawingAddress);
  CHECK(std::holds_alternative<ComponentTreeBodyAddress>(
      bodyAddress->get().value()));
  CHECK(std::holds_alternative<ComponentTreeSketchAddress>(
      sketchAddress->get().value()));
  CHECK(std::holds_alternative<ComponentTreeReferenceImageAddress>(
      imageAddress->get().value()));
  CHECK(std::holds_alternative<ComponentTreeDrawingAddress>(
      drawingAddress->get().value()));
  CHECK(std::get<ComponentTreeBodyAddress>(bodyAddress->get().value())
            .body.bodyId == 7);
  CHECK(std::get<ComponentTreeSketchAddress>(sketchAddress->get().value())
            .sketchId == 7);
  CHECK(std::get<ComponentTreeReferenceImageAddress>(
            imageAddress->get().value())
            .imageId == 7);
  CHECK(std::get<ComponentTreeDrawingAddress>(drawingAddress->get().value())
            .drawingId == DrawingId{7});

  const QModelIndex left = model->index(5, 0, root);
  const QModelIndex right = model->index(6, 0, root);
  const auto leftAddress = model->addressForIndex(left);
  const auto rightAddress = model->addressForIndex(right);
  CHECK(leftAddress && rightAddress);
  const auto& leftOccurrence =
      std::get<ComponentTreeOccurrenceAddress>(leftAddress->get().value());
  const auto& rightOccurrence =
      std::get<ComponentTreeOccurrenceAddress>(rightAddress->get().value());
  CHECK(leftOccurrence.instance.occurrencePath->entries() ==
        std::vector<OccurrenceId>{OccurrenceId{10}});
  CHECK(rightOccurrence.instance.occurrencePath->entries() ==
        std::vector<OccurrenceId>{OccurrenceId{20}});
  CHECK(model->data(left).toString().contains(
      QString::fromUtf8("Левая ветвь")));
  CHECK(model->data(right).toString().contains(
      QString::fromUtf8("Правая ветвь")));
  CHECK(!model->data(right, kComponentTreeAuthoredVisibleRole).toBool());
  CHECK(model->data(left, Qt::CheckStateRole).toInt() == Qt::Checked);
  CHECK(model->data(right, Qt::CheckStateRole).toInt() == Qt::Unchecked);
  CHECK(model->data(right, kComponentTreeAuthoredSuppressedRole).toBool());
  CHECK(!model->data(right, kComponentTreeEffectiveVisibleRole).toBool());
  CHECK(model->data(right, kComponentTreeEffectiveSuppressedRole).toBool());

  CHECK(model->rowCount(left) == 6);
  CHECK(model->rowCount(right) == 6);
  const QModelIndex leftLeaf = model->index(5, 0, left);
  const QModelIndex rightLeaf = model->index(5, 0, right);
  const auto leftLeafAddress = model->addressForIndex(leftLeaf);
  const auto rightLeafAddress = model->addressForIndex(rightLeaf);
  CHECK(leftLeafAddress && rightLeafAddress);
  CHECK(std::get<ComponentTreeOccurrenceAddress>(
            leftLeafAddress->get().value())
            .instance.occurrencePath->entries() ==
        std::vector<OccurrenceId>({OccurrenceId{10}, OccurrenceId{7}}));
  CHECK(std::get<ComponentTreeOccurrenceAddress>(
            rightLeafAddress->get().value())
            .instance.occurrencePath->entries() ==
        std::vector<OccurrenceId>({OccurrenceId{20}, OccurrenceId{7}}));
  CHECK(model->data(leftLeaf, kComponentTreeAuthoredGroundedRole).toBool());
  CHECK(model->data(rightLeaf, kComponentTreeAuthoredGroundedRole).toBool());
  CHECK(!model->data(rightLeaf, kComponentTreeEffectiveVisibleRole).toBool());
  CHECK(model->data(rightLeaf, kComponentTreeEffectiveSuppressedRole).toBool());

  // Shared definition-local IDs remain distinct because every entity address
  // carries its complete component instance address.
  const QModelIndex leftBodies = section(
      *model, left, ComponentTreeGroupKind::Bodies);
  const QModelIndex rightBodies = section(
      *model, right, ComponentTreeGroupKind::Bodies);
  const auto leftBody =
      model->addressForIndex(model->index(0, 0, leftBodies));
  const auto rightBody =
      model->addressForIndex(model->index(0, 0, rightBodies));
  CHECK(leftBody && rightBody);
  const auto& leftBodyValue =
      std::get<ComponentTreeBodyAddress>(leftBody->get().value()).body;
  const auto& rightBodyValue =
      std::get<ComponentTreeBodyAddress>(rightBody->get().value()).body;
  CHECK(leftBodyValue.bodyId == rightBodyValue.bodyId);
  CHECK(leftBodyValue.component.definitionId ==
        rightBodyValue.component.definitionId);
  CHECK(leftBodyValue.component.occurrencePath !=
        rightBodyValue.component.occurrencePath);
  const QModelIndex leftDrawings = section(
      *model, left, ComponentTreeGroupKind::Drawings);
  const QModelIndex rightDrawings = section(
      *model, right, ComponentTreeGroupKind::Drawings);
  const auto leftDrawing =
      model->addressForIndex(model->index(0, 0, leftDrawings));
  const auto rightDrawing =
      model->addressForIndex(model->index(0, 0, rightDrawings));
  CHECK(leftDrawing && rightDrawing);
  const auto& leftDrawingValue =
      std::get<ComponentTreeDrawingAddress>(leftDrawing->get().value());
  const auto& rightDrawingValue =
      std::get<ComponentTreeDrawingAddress>(rightDrawing->get().value());
  CHECK(leftDrawingValue.drawingId == rightDrawingValue.drawingId);
  CHECK(leftDrawingValue.instance.occurrencePath !=
        rightDrawingValue.instance.occurrencePath);

  const QModelIndex unused = model->index(1, 0);
  CHECK(model->data(unused).toString() ==
        QString::fromUtf8("Неиспользуемые компоненты"));
  CHECK(std::holds_alternative<ComponentTreeUnusedSectionAddress>(
      model->addressForIndex(unused)->get().value()));
  CHECK(model->rowCount(unused) == 1);
  const QModelIndex unusedDefinition = model->index(0, 0, unused);
  const auto unusedAddress = model->addressForIndex(unusedDefinition);
  CHECK(unusedAddress);
  CHECK(std::get<ComponentTreeUnusedDefinitionAddress>(
            unusedAddress->get().value())
            .definitionId == DefinitionId{400});
  CHECK(model->data(unusedDefinition,
                    kComponentTreeUnusedDefinitionRole).toBool());
  CHECK(model->rowCount(unusedDefinition) == 0);
  CHECK(model->flags(unused) & Qt::ItemIsUserCheckable);
  CHECK(model->flags(unusedDefinition) & Qt::ItemIsUserCheckable);
  const auto* unusedProjection =
      coordinator.active().snapshotIfCurrent()->treeProjection().findDefinition(
          DefinitionId{400});
  CHECK(unusedProjection);
  CHECK(unusedProjection->drawings.size() == 1);
  CHECK(unusedProjection->drawings.front().nameUtf8 ==
        "unused drawing first");

  verifyRoundTripRecursively(*model);
  CHECK(model->flags(left) ==
        (Qt::ItemIsEnabled | Qt::ItemIsSelectable |
         Qt::ItemIsUserCheckable | Qt::ItemIsEditable));
  CHECK(!(model->flags(left) & Qt::ItemIsDragEnabled));
  CHECK(!(model->flags(left) & Qt::ItemIsDropEnabled));
  CHECK(!(model->flags(bodies) & Qt::ItemIsEditable));
  CHECK(!model->setData(bodies, QStringLiteral("rename")));
  CHECK(model->data(left, Qt::EditRole).toString() ==
        QString::fromUtf8("Левая ветвь"));
  std::optional<ComponentTreeAddressValue> renameRequest;
  QString requestedName;
  QObject::connect(
      model.get(), &ComponentTreeModel::nodeRenameRequested,
      [&renameRequest, &requestedName](ComponentTreeAddressValue address,
                                       const QString& name) {
        renameRequest = std::move(address);
        requestedName = name;
      });
  CHECK(model->setData(left, QString::fromUtf8("Новая ветвь"),
                       Qt::EditRole));
  CHECK(renameRequest);
  CHECK(std::holds_alternative<ComponentTreeOccurrenceAddress>(
      *renameRequest));
  CHECK(requestedName == QString::fromUtf8("Новая ветвь"));
  // The immutable projection does not publish an optimistic label. The owner
  // commits the rename and replaces the projection; on failure the old name
  // remains authoritative instead of leaving a false UI-only rename behind.
  CHECK(model->data(left, Qt::EditRole).toString() ==
        QString::fromUtf8("Левая ветвь"));
  std::optional<ComponentInstanceAddress> visibilityRequest;
  bool requestedVisible = true;
  QObject::connect(
      model.get(), &ComponentTreeModel::componentVisibilityChangeRequested,
      [&visibilityRequest, &requestedVisible](ComponentInstanceAddress instance,
                                             bool visible) {
        visibilityRequest = std::move(instance);
        requestedVisible = visible;
      });
  CHECK(model->setData(left, Qt::Unchecked, Qt::CheckStateRole));
  CHECK(visibilityRequest);
  CHECK(!requestedVisible);
  CHECK(*visibilityRequest == leftOccurrence.instance);
  std::optional<ComponentTreeAddressValue> nodeVisibilityRequest;
  QObject::connect(
      model.get(), &ComponentTreeModel::nodeVisibilityChangeRequested,
      [&nodeVisibilityRequest](ComponentTreeAddressValue address, bool visible) {
        CHECK(!visible);
        nodeVisibilityRequest = std::move(address);
      });
  CHECK(model->setData(bodies, Qt::Unchecked, Qt::CheckStateRole));
  CHECK(nodeVisibilityRequest);
  CHECK(std::holds_alternative<ComponentTreeGroupAddress>(
      *nodeVisibilityRequest));
  CHECK(model->data(bodies, Qt::CheckStateRole).toInt() == Qt::Unchecked);
  CHECK(model->data(model->index(0, 0, bodies), Qt::CheckStateRole).toInt() ==
        Qt::Unchecked);
  CHECK(model->data(model->index(1, 0, bodies), Qt::CheckStateRole).toInt() ==
        Qt::Unchecked);
  CHECK(!model->insertRows(0, 1));
  CHECK(!model->removeRows(0, 1));
  CHECK(model->supportedDropActions() == Qt::IgnoreAction);
  CHECK(model->supportedDragActions() == Qt::IgnoreAction);

  // Same numeric generation from another coordinator is a foreign address.
  ComponentScenePublicationCoordinator foreignCoordinator;
  CHECK(foreignCoordinator.rebuildAndPublish(*fixture.product, kEpoch, kNormal));
  CHECK(foreignCoordinator.active().generation().value() ==
        coordinator.active().generation().value());
  auto foreignModel = ComponentTreeModel::create(
      *fixture.product, foreignCoordinator.active());
  CHECK(foreignModel);
  const auto foreignRoot =
      foreignModel->addressForIndex(foreignModel->index(0, 0));
  CHECK(foreignRoot);
  CHECK(!model->indexForAddress(foreignRoot->get()).isValid());

  const auto oldRootAddress = model->addressForIndex(root);
  CHECK(oldRootAddress);
  const auto oldPublication = coordinator.active();
  CHECK(coordinator.rebuildAndPublish(
      *fixture.product, ProductPresentationEpoch{7102}, kNormal));
  CHECK(!model->indexForAddress(oldRootAddress->get()).isValid());
  CHECK(model->addressForIndex(root));
  const auto staleCreation = ComponentTreeModel::createTyped(
      *fixture.product, oldPublication);
  CHECK(staleCreation.status == ComponentTreeModelCreateStatus::Rejected);
  CHECK(!staleCreation.model);
}

void verifyComponentVisibilityAndDefinitionEditing() {
  auto fixture = graphProduct();
  auto path = OccurrencePath::create({OccurrenceId{10}});
  CHECK(path);
  const DefinitionStateId visibilityBefore =
      fixture.product->definitionStateId(DefinitionId{300});
  AcceptSuccessor successor;
  CHECK(fixture.product->setComponentOccurrenceVisible(
            *path.path, false, successor) ==
        ComponentVisibilityStatus::Committed);
  const auto* root = fixture.product->rootComponentDefinition();
  CHECK(root);
  CHECK(!root->find(OccurrenceId{10})->flags().visible);
  CHECK(root->stateId() != visibilityBefore);
  CHECK(fixture.product->setComponentOccurrenceVisible(
            *path.path, false, successor) ==
        ComponentVisibilityStatus::NoChange);

  const DefinitionStateId editBefore = root->stateId();
  Document candidate = root->document();
  candidate.addBody(BodyId{99}, "active component body");
  CHECK(candidate.recompute());
  const auto edited = fixture.product->applyDefinitionEdit(
      {fixture.product->incarnation(), fixture.product->rootDefinitionId(),
       DefinitionId{300}, editBefore},
      candidate, successor);
  CHECK(edited.status == DefinitionEditStatus::Committed);
  root = fixture.product->rootComponentDefinition();
  CHECK(root);
  CHECK(root->document().findBody(BodyId{99}));
  CHECK(!candidate.findBody(BodyId{99}));
  CHECK(root->stateId() == edited.committedStateId);

  RejectSuccessor rejected;
  CHECK(fixture.product->renameComponentOccurrence(
            *path.path, "Не должно сохраниться", rejected) ==
        ComponentOccurrenceEditStatus::SuccessorRejected);
  root = fixture.product->rootComponentDefinition();
  CHECK(root && root->find(OccurrenceId{10}));
  CHECK(root->find(OccurrenceId{10})->displayNameUtf8() == "Левая ветвь");
  CHECK(fixture.product->removeComponentOccurrence(*path.path, rejected) ==
        ComponentOccurrenceEditStatus::SuccessorRejected);
  root = fixture.product->rootComponentDefinition();
  CHECK(root && root->find(OccurrenceId{10}));

  CHECK(fixture.product->renameComponentOccurrence(
            *path.path, "Переименованный компонент", successor) ==
        ComponentOccurrenceEditStatus::Committed);
  root = fixture.product->rootComponentDefinition();
  CHECK(root);
  CHECK(root->find(OccurrenceId{10}));
  CHECK(root->find(OccurrenceId{10})->displayNameUtf8() ==
        "Переименованный компонент");
  CHECK(fixture.product->removeComponentOccurrence(*path.path, successor) ==
        ComponentOccurrenceEditStatus::Committed);
  root = fixture.product->rootComponentDefinition();
  CHECK(root);
  CHECK(!root->find(OccurrenceId{10}));
}

void verifyModelOutlivesProduct() {
  auto fixture = graphProduct();
  ComponentScenePublicationCoordinator coordinator;
  CHECK(coordinator.rebuildAndPublish(*fixture.product, kEpoch, kNormal));
  auto model = ComponentTreeModel::create(
      *fixture.product, coordinator.active());
  CHECK(model);
  const QModelIndex root = model->index(0, 0);
  const auto rootAddress = model->addressForIndex(root);
  CHECK(rootAddress);
  const QString label = model->data(root).toString();
  fixture.product.reset();
  CHECK(model->data(root).toString() == label);
  CHECK(model->indexForAddress(rootAddress->get()) == root);
  verifyRoundTripRecursively(*model);
}

ComponentDefinition& mutableComponent(ProductDocument& product,
                                      DefinitionId id) {
  auto* component = const_cast<ComponentDefinition*>(
      product.definitions().findComponent(id));
  CHECK(component);
  return *component;
}

ReferenceImage additionalImage() {
  ReferenceImage image;
  image.id = 99;
  image.name = "added image";
  image.sourcePath = "added.png";
  image.supportName = "YZ";
  image.pixelWidth = 4;
  image.pixelHeight = 4;
  return image;
}

void verifyPostPublicationDriftRejected() {
  const auto verifyRejected = [](const std::function<void(ProductDocument&)>&
                                     mutate) {
    auto fixture = graphProduct();
    ComponentScenePublicationCoordinator coordinator;
    CHECK(coordinator.rebuildAndPublish(*fixture.product, kEpoch, kNormal));
    const auto publication = coordinator.active();
    auto incumbent = ComponentTreeModel::create(
        *fixture.product, publication);
    CHECK(incumbent);
    const QModelIndex incumbentRoot = incumbent->index(0, 0);
    const QString incumbentLabel = incumbent->data(incumbentRoot).toString();
    mutate(*fixture.product);
    CHECK(incumbent->data(incumbentRoot).toString() == incumbentLabel);
    CHECK(incumbent->addressForIndex(incumbentRoot));
    const auto rejected = ComponentTreeModel::createTyped(
        *fixture.product, publication);
    CHECK(rejected.status == ComponentTreeModelCreateStatus::Rejected);
    CHECK(!rejected.model);
    CHECK(publication.isCurrent());
  };

  verifyRejected([](ProductDocument& product) {
    mutableComponent(product, DefinitionId{300})
        .document()
        .addBody(BodyId{99}, "added body");
  });
  verifyRejected([](ProductDocument& product) {
    CHECK(mutableComponent(product, DefinitionId{300})
              .document()
              .applyBodySlice(0, std::nullopt));
  });
  verifyRejected([](ProductDocument& product) {
    auto& document =
        mutableComponent(product, DefinitionId{300}).document();
    document.findBody(BodyId{7})->setName("changed body");
  });
  verifyRejected([](ProductDocument& product) {
    mutableComponent(product, DefinitionId{300})
        .document()
        .addSketch(SketchId{99}, "added sketch");
  });
  verifyRejected([](ProductDocument& product) {
    CHECK(mutableComponent(product, DefinitionId{300})
              .document()
              .applySketchSlice(0, std::nullopt));
  });
  verifyRejected([](ProductDocument& product) {
    auto* sketch = mutableComponent(product, DefinitionId{300})
                       .document()
                       .sketchAt(0);
    CHECK(sketch);
    sketch->name = "changed sketch";
  });
  verifyRejected([](ProductDocument& product) {
    mutableComponent(product, DefinitionId{300})
        .document()
        .addReferenceImage(additionalImage());
  });
  verifyRejected([](ProductDocument& product) {
    CHECK(mutableComponent(product, DefinitionId{300})
              .document()
              .applyReferenceImageSlice(0, std::nullopt));
  });
  verifyRejected([](ProductDocument& product) {
    auto* image = mutableComponent(product, DefinitionId{300})
                      .document()
                      .findReferenceImage(ReferenceImageId{7});
    CHECK(image);
    image->name = "changed image";
    image->visible = !image->visible;
  });
  verifyRejected([](ProductDocument& product) {
    CHECK(mutableComponent(product, DefinitionId{300})
              .insert(ComponentOccurrence(
                  OccurrenceId{30}, DefinitionId{200},
                  RigidTransform3d::identity())) ==
          ComponentOccurrenceInsertStatus::Inserted);
  });
  verifyRejected([](ProductDocument& product) {
    CHECK(mutableComponent(product, DefinitionId{300})
              .erase(OccurrenceId{10}) ==
          ComponentOccurrenceEraseStatus::Erased);
  });
  verifyRejected([](ProductDocument& product) {
    auto& root = mutableComponent(product, DefinitionId{300});
    const auto* old = root.find(OccurrenceId{10});
    CHECK(old);
    const DefinitionId target = old->targetDefinitionId();
    const auto placement = old->placement();
    const auto flags = old->flags();
    CHECK(root.erase(OccurrenceId{10}) ==
          ComponentOccurrenceEraseStatus::Erased);
    CHECK(root.insert(ComponentOccurrence(
              OccurrenceId{10}, target, placement, flags,
              "changed occurrence")) ==
          ComponentOccurrenceInsertStatus::Inserted);
  });
  verifyRejected([](ProductDocument& product) {
    auto& registry =
        const_cast<DefinitionRegistry&>(product.definitions());
    CHECK(registry.insert(ComponentDefinition(
              DefinitionId{500}, std::make_unique<Document>())) ==
          DefinitionInsertStatus::Inserted);
  });
  verifyRejected([](ProductDocument& product) {
    auto& registry =
        const_cast<DefinitionRegistry&>(product.definitions());
    CHECK(registry.erase(DefinitionId{400}) ==
          DefinitionEraseStatus::Erased);
  });
  verifyRejected([](ProductDocument& product) {
    auto& unused = mutableComponent(product, DefinitionId{400});
    unused.document().findBody(BodyId{7})->setName("changed unused body");
  });
  verifyRejected([](ProductDocument& product) {
    ComponentDrawingValidationTestAccess::append(
        mutableComponent(product, DefinitionId{400}),
        ComponentDrawing(
            DrawingId{99}, "added unused drawing", ComponentDrawingPaper::A4,
            ComponentDrawingOrientation::Landscape, 4.0,
            std::vector<BodyId>{BodyId{3}}));
  });

  // Identical authored IDs and labels are not enough: a different product
  // incarnation cannot satisfy the publication's complete projection.
  auto published = graphProduct();
  auto foreign = graphProduct();
  ComponentScenePublicationCoordinator coordinator;
  CHECK(coordinator.rebuildAndPublish(*published.product, kEpoch, kNormal));
  const auto foreignRejected = ComponentTreeModel::createTyped(
      *foreign.product, coordinator.active());
  CHECK(foreignRejected.status == ComponentTreeModelCreateStatus::Rejected);
  CHECK(!foreignRejected.model);
  CHECK(published.product->incarnation() != foreign.product->incarnation());
  const auto publishedSnapshot = ComponentPresentationBuilder::build(
      *published.product, kEpoch, kNormal);
  const auto foreignSnapshot = ComponentPresentationBuilder::build(
      *foreign.product, kEpoch, kNormal);
  CHECK(publishedSnapshot && foreignSnapshot);
  CHECK(publishedSnapshot.snapshot->treeProjection().productIncarnation !=
        foreignSnapshot.snapshot->treeProjection().productIncarnation);
  CHECK(!publishedSnapshot.snapshot->semanticallyEquivalent(
      *foreignSnapshot.snapshot));
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  verifyHierarchyOrderingAddressesAndRoles();
  verifyComponentVisibilityAndDefinitionEditing();
  verifyModelOutlivesProduct();
  verifyPostPublicationDriftRejected();
  return EXIT_SUCCESS;
}
