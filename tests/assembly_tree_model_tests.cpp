#include "TestAssertions.h"

#include <QCoreApplication>

#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "model/ProductDocument.h"
#include "model/OccurrenceRemove.h"
#include "ui/AssemblyTreeModel.h"
#include "ui/AssemblyViewportSession.h"

namespace {

using namespace solidar;

static_assert(std::is_same_v<
              AssemblyTreeModel::AddressView,
              std::optional<
                  std::reference_wrapper<const AssemblyTreeNodeAddress>>>);
static_assert(noexcept(std::declval<const AssemblyTreeModel&>()
                           .addressForIndex(std::declval<const QModelIndex&>())));

std::unique_ptr<ProductDocument> product() {
  DefinitionRegistry definitions;
  ProjectId::Bytes projectBytes{};
  projectBytes[0] = 0x42;
  SourceContentHash hash;
  hash.bytes[0] = 0x24;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{20}, std::make_unique<Document>(),
            {{ProjectId{projectBytes}, DefinitionId{7}},
             "parts/source.solidar", hash})) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(PartDefinition(DefinitionId{10},
                                          std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(PartDefinition(DefinitionId{30},
                                          std::make_unique<Document>())) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{100});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{2}, DefinitionId{20}, RigidTransform3d::identity(),
            {.visible = false, .suppressed = true, .grounded = true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, DefinitionId{10},
            RigidTransform3d::identity(), {},
            std::string("Кронштейн 🔩"))) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{100});
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> nestedProduct() {
  std::vector<PartDefinition> parts;
  parts.emplace_back(DefinitionId{10}, std::make_unique<Document>());
  parts.emplace_back(DefinitionId{30}, std::make_unique<Document>());
  std::vector<AssemblyDefinition> assemblies;
  AssemblyDefinition child(DefinitionId{200});
  CHECK(child.insert(ComponentOccurrence(
            OccurrenceId{7}, DefinitionId{10}, RigidTransform3d::identity(),
            {}, "Лист")) == AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition orphan(DefinitionId{300});
  AssemblyDefinition root(DefinitionId{100});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, DefinitionId{200}, RigidTransform3d::identity(),
            {}, "Левая ветвь")) == AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{2}, DefinitionId{200}, RigidTransform3d::identity(),
            {}, "Правая ветвь")) == AssemblyOccurrenceInsertStatus::Inserted);
  assemblies.push_back(std::move(child));
  assemblies.push_back(std::move(orphan));
  assemblies.push_back(std::move(root));
  auto made = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), DefinitionId{100});
  CHECK(made);
  return std::move(made.product);
}

class AcceptSuccessor final : public ProductDocumentSuccessor {
 public:
  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&,
      ProductDocumentSuccessorImpact) noexcept override {
    return ProductDocumentSuccessorStatus::Prepared;
  }
};

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  auto value = product();
  constexpr ProductPresentationEpoch epoch{9};
  auto session = AssemblyViewportSession::prepare(
      *value, epoch, AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(session);
  const auto resourceFailure = AssemblyTreeModel::createTyped(
      *value, epoch, session.session->publicationHandle(), nullptr,
      AssemblyTreeModelFailurePoint::ResourceFailure);
  CHECK(resourceFailure.status ==
        AssemblyTreeModelCreateStatus::ResourceFailure);
  CHECK(!resourceFailure.model);
  const LinkedPartRuntimeStates linkedStates{
      {DefinitionId{20}, {LinkedPartRuntimeStatus::Outdated, {}}}};
  auto model = AssemblyTreeModel::create(
      *value, epoch, session.session->publicationHandle(), nullptr,
      std::nullopt, &linkedStates);
  CHECK(model);
  CHECK(model->columnCount() == 1);
  CHECK(model->rowCount() == 4);  // root + all three part definitions
  const QModelIndex linkedDefinition =
      model->indexForDefinition(DefinitionId{20});
  CHECK(linkedDefinition.isValid());
  CHECK(model->data(linkedDefinition).toString().contains(
      QString::fromUtf8("источник изменён")));

  const QModelIndex root = model->index(0, 0);
  CHECK(root.isValid());
  CHECK(model->indexForDefinition(DefinitionId{100}) == root);
  CHECK(model->data(root).toString().contains(QString::fromUtf8("Сборка")));
  CHECK(!model->data(root).toString().contains(
      QString::fromUtf8("только чтение")));
  const auto rootAddress = model->addressForIndex(root);
  CHECK(rootAddress);
  CHECK(rootAddress->get().kind ==
        AssemblyTreeNodeKind::RootAssemblyDefinition);
  CHECK(rootAddress->get().definitionId == DefinitionId{100});
  CHECK(model->rowCount(root) == 2);

  // Occurrences and definitions are deterministic ID order; orphan 30 remains.
  const auto firstOccurrence = model->addressForIndex(model->index(0, 0, root));
  const auto secondOccurrence = model->addressForIndex(model->index(1, 0, root));
  CHECK(firstOccurrence && secondOccurrence);
  CHECK(firstOccurrence->get()
            .occurrence->occurrence.path.entries().front() ==
        OccurrenceId{1});
  CHECK(secondOccurrence->get()
            .occurrence->occurrence.path.entries().front() ==
        OccurrenceId{2});
  CHECK(model->data(model->index(0, 0, root)).toString().contains(
      QString::fromUtf8("Кронштейн 🔩")));
  CHECK(model->data(model->index(1, 0, root)).toString().contains(
      QString::fromUtf8("Экземпляр 2")));
  CHECK(model->data(model->index(1, 0, root)).toString().contains(
      QString::fromUtf8("скрытый")));
  CHECK(model->data(model->index(1, 0, root)).toString().contains(
      QString::fromUtf8("подавлен")));
  CHECK(model->data(model->index(1, 0, root)).toString().contains(
      QString::fromUtf8("зафиксирован")));
  CHECK(!model->data(model->index(0, 0, root),
                     kAssemblyTreeAuthoredSuppressedRole)
             .toBool());
  CHECK(!model->data(model->index(1, 0, root),
                     kAssemblyTreeAuthoredVisibleRole)
             .toBool());
  CHECK(model->data(model->index(1, 0, root),
                    kAssemblyTreeAuthoredSuppressedRole)
            .toBool());
  CHECK(!model->data(model->index(0, 0, root),
                     kAssemblyTreeAuthoredGroundedRole)
             .toBool());
  CHECK(model->data(model->index(1, 0, root),
                    kAssemblyTreeAuthoredGroundedRole)
            .toBool());

  CHECK(model->addressForIndex(model->index(1, 0))->get().definitionId ==
        DefinitionId{10});
  CHECK(model->addressForIndex(model->index(2, 0))->get().definitionId ==
        DefinitionId{20});
  CHECK(model->addressForIndex(model->index(3, 0))->get().definitionId ==
        DefinitionId{30});
  CHECK(!model->data(model->index(1, 0),
                     kAssemblyTreeOrphanDefinitionRole)
             .toBool());
  CHECK(!model->data(model->index(2, 0),
                     kAssemblyTreeOrphanDefinitionRole)
             .toBool());
  CHECK(model->data(model->index(3, 0),
                    kAssemblyTreeOrphanDefinitionRole)
            .toBool());
  CHECK(model->data(model->index(3, 0)).toString() ==
        QString::fromUtf8("Деталь 30 (не используется)"));

  const QModelIndex found = model->indexForOccurrence(
      *secondOccurrence->get().occurrence);
  CHECK(found == model->index(1, 0, root));
  auto foreignSession = AssemblyViewportSession::prepare(
      *value, epoch, AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(foreignSession);
  AssemblyOccurrenceSelection foreign{
      foreignSession.session->publicationHandle(),
      secondOccurrence->get().occurrence->occurrence};
  CHECK(!model->indexForOccurrence(foreign).isValid());
  CHECK(model->flags(found) == (Qt::ItemIsEnabled | Qt::ItemIsSelectable));
  CHECK(!model->setData(found, QStringLiteral("rename")));
  CHECK(!model->insertRows(0, 1));
  CHECK(!model->removeRows(0, 1));
  CHECK(model->supportedDropActions() == Qt::IgnoreAction);

  auto isolated = AssemblyTreeModel::create(
      *value, epoch, session.session->publicationHandle(), nullptr,
      *OccurrencePath::create({OccurrenceId{1}}).path);
  CHECK(isolated);
  const QModelIndex isolatedRoot = isolated->index(0, 0);
  CHECK(isolated->data(isolated->index(0, 0, isolatedRoot))
            .toString()
            .contains(QString::fromUtf8("изолирован")));
  CHECK(isolated->data(isolated->index(0, 0, isolatedRoot),
                       kAssemblyTreeIsolationStateRole)
            .toInt() ==
        static_cast<int>(AssemblyTreeIsolationState::Isolated));
  CHECK(isolated->data(isolated->index(1, 0, isolatedRoot))
            .toString()
            .contains(QString::fromUtf8("временно скрыт (изоляция)")));
  CHECK(isolated->data(isolated->index(1, 0, isolatedRoot))
            .toString()
            .contains(QString::fromUtf8("скрытый")));
  CHECK(isolated->data(isolated->index(1, 0, isolatedRoot),
                       kAssemblyTreeIsolationStateRole)
            .toInt() ==
        static_cast<int>(AssemblyTreeIsolationState::TemporarilyHidden));

  // Removing the only occurrence of definition 20 retains its definition row
  // and marks it orphan. Restoring the exact occurrence clears only that
  // semantic marker.
  auto occurrencePath = OccurrencePath::create({OccurrenceId{2}});
  CHECK(occurrencePath);
  const auto* removable =
      value->rootAssemblyDefinition()->find(OccurrenceId{2});
  CHECK(removable);
  RemoveOccurrenceColdPayload payload{
      {value->incarnation(), value->rootDefinitionId(), *occurrencePath.path,
       removable->targetDefinitionId()},
      removable->placement(), removable->flags(),
      removable->displayNameUtf8(), removable->homePlacement()};
  AcceptSuccessor accept;
  CHECK(value->applyRemoveOccurrence(
            payload, accept, RemoveOccurrenceFailurePoint::None) ==
        RemoveOccurrenceStatus::Removed);
  auto removedSession = AssemblyViewportSession::prepare(
      *value, epoch, AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(removedSession);
  auto removedTree = AssemblyTreeModel::create(
      *value, epoch, removedSession.session->publicationHandle());
  CHECK(removedTree);
  CHECK(removedTree->rowCount() == 4);
  CHECK(removedTree->data(removedTree->index(2, 0),
                          kAssemblyTreeOrphanDefinitionRole)
            .toBool());
  CHECK(removedTree->data(removedTree->index(2, 0)).toString().contains(
      QString::fromUtf8("Деталь 20 (не используется)")));
  CHECK(removedTree->data(removedTree->index(2, 0)).toString().contains(
      QString::fromUtf8("связь не проверена")));
  CHECK(value->restoreRemovedOccurrence(
            payload, accept, RemoveOccurrenceFailurePoint::None) ==
        RemoveOccurrenceStatus::Restored);
  auto restoredSession = AssemblyViewportSession::prepare(
      *value, epoch, AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(restoredSession);
  auto restoredTree = AssemblyTreeModel::create(
      *value, epoch, restoredSession.session->publicationHandle());
  CHECK(restoredTree);
  CHECK(!restoredTree->data(restoredTree->index(2, 0),
                            kAssemblyTreeOrphanDefinitionRole)
             .toBool());
  CHECK(restoredTree->data(restoredTree->index(2, 0)).toString().contains(
      QString::fromUtf8("Деталь 20")));
  CHECK(restoredTree->data(restoredTree->index(2, 0)).toString().contains(
      QString::fromUtf8("связь не проверена")));

  // Recursive occurrences are true QModelIndex hierarchy and the same local
  // child occurrence id remains distinct under each full-path prefix.
  auto nested = nestedProduct();
  auto nestedSession = AssemblyViewportSession::prepare(
      *nested, ProductPresentationEpoch{10},
      AssemblyMeshProfile{AssemblyMeshQuality::Normal});
  CHECK(nestedSession);
  auto nestedTree = AssemblyTreeModel::create(
      *nested, ProductPresentationEpoch{10},
      nestedSession.session->publicationHandle());
  CHECK(nestedTree);
  CHECK(nestedTree->rowCount() == 5);  // root + four non-root definitions
  const QModelIndex nestedRoot = nestedTree->index(0, 0);
  CHECK(nestedTree->rowCount(nestedRoot) == 2);
  const QModelIndex left = nestedTree->index(0, 0, nestedRoot);
  const QModelIndex right = nestedTree->index(1, 0, nestedRoot);
  CHECK(nestedTree->data(left).toString().contains(
      QString::fromUtf8("Левая ветвь → подсборка 200")));
  CHECK(nestedTree->data(right).toString().contains(
      QString::fromUtf8("Правая ветвь → подсборка 200")));
  CHECK(nestedTree->rowCount(left) == 1);
  CHECK(nestedTree->rowCount(right) == 1);
  const QModelIndex leftLeaf = nestedTree->index(0, 0, left);
  const QModelIndex rightLeaf = nestedTree->index(0, 0, right);
  const auto leftAddress = nestedTree->addressForIndex(leftLeaf);
  const auto rightAddress = nestedTree->addressForIndex(rightLeaf);
  CHECK(leftAddress && rightAddress);
  CHECK(leftAddress->get().occurrence->occurrence.path.entries() ==
        std::vector<OccurrenceId>({OccurrenceId{1}, OccurrenceId{7}}));
  CHECK(rightAddress->get().occurrence->occurrence.path.entries() ==
        std::vector<OccurrenceId>({OccurrenceId{2}, OccurrenceId{7}}));
  CHECK(nestedTree->indexForOccurrence(
            *leftAddress->get().occurrence) == leftLeaf);
  CHECK(nestedTree->indexForOccurrence(
            *rightAddress->get().occurrence) == rightLeaf);
  const QModelIndex childDefinition =
      nestedTree->indexForDefinition(DefinitionId{200});
  const QModelIndex orphanAssembly =
      nestedTree->indexForDefinition(DefinitionId{300});
  const QModelIndex orphanPart =
      nestedTree->indexForDefinition(DefinitionId{30});
  CHECK(childDefinition.isValid() && orphanAssembly.isValid() &&
        orphanPart.isValid());
  CHECK(!nestedTree->data(childDefinition, kAssemblyTreeOrphanDefinitionRole)
             .toBool());
  CHECK(nestedTree->data(orphanAssembly, kAssemblyTreeOrphanDefinitionRole)
            .toBool());
  CHECK(nestedTree->data(orphanPart, kAssemblyTreeOrphanDefinitionRole)
            .toBool());
  CHECK(nestedTree->data(orphanAssembly).toString().contains(
      QString::fromUtf8("Подсборка 300 (не используется)")));
  return EXIT_SUCCESS;
}
