#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>

#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include "app/AppSettings.h"
#include "model/ImportedShapeFeature.h"
#include "model/ProductDocument.h"
#include "ui/ComponentWorkspace.h"
#include "ui/MainWindow.h"
#include "ui/ModelRibbon.h"
#include "ui/Viewport.h"

namespace solidar {

class ComponentWorkspaceMainWindowTestAccess final {
 public:
  static bool installComponent(MainWindow& window, ProductDocument product) {
    auto prepared = window.prepareComponentWorkspace(StagedApplicationDocument(
        std::move(product), std::nullopt, std::nullopt));
    if (!prepared) return false;
    window.commitComponentReplacement(std::move(prepared), QString{});
    return true;
  }

  static void installPart(MainWindow& window) {
    window.commitDocumentReplacement(
        StagedApplicationDocument(Document{}, std::nullopt, std::nullopt),
        MainWindow::DocumentReplacementOrigin::ImportedStep,
        QStringLiteral("part"));
  }

  static bool installAssembly(MainWindow& window, ProductDocument product) {
    auto prepared = window.prepareAssemblyWorkspace(StagedApplicationDocument(
        std::move(product), std::nullopt, std::nullopt));
    if (!prepared) return false;
    window.commitAssemblyReplacement(std::move(prepared), QString{});
    return true;
  }

  static Viewport* viewport(MainWindow& window) { return window.viewport_; }
  static QTreeView* componentTree(MainWindow& window) {
    return window.componentTreeView_;
  }
  static ComponentTreeModel* componentModel(MainWindow& window) {
    return window.componentTreeModel_;
  }
  static QWidget* currentRibbon(MainWindow& window) {
    return window.ribbonStack_->currentWidget();
  }
  static QWidget* currentTree(MainWindow& window) {
    return window.modelTreeStack_->currentWidget();
  }
  static ModelRibbon* modelRibbon(MainWindow& window) {
    return window.modelRibbon_;
  }
  static QWidget* assemblyRibbon(MainWindow& window) {
    return window.assemblyRibbon_;
  }
  static void failComponentPreparation(MainWindow& window) {
    window.componentPreparationFailurePoint_ =
        ComponentViewportSessionFailurePoint::Resource;
  }
  static void clearComponentPreparationFailure(MainWindow& window) {
    window.componentPreparationFailurePoint_ =
        ComponentViewportSessionFailurePoint::None;
  }
  static bool prepareComponent(MainWindow& window, ProductDocument product) {
    return window.prepareComponentWorkspace(StagedApplicationDocument(
               std::move(product), std::nullopt, std::nullopt)) != nullptr;
  }
  static bool rebuildComponentProjection(MainWindow& window) {
    const ComponentWorkspaceInstanceKey root{
        window.productDocument_.rootDefinitionId(), std::nullopt};
    const ComponentWorkspaceSemanticState semantic{root, root, std::nullopt,
                                                   {}};
    auto prepared = ComponentProjectionPreparer::prepare(
        window.productDocument_,
        ProductPresentationEpoch{window.componentPresentationEpochClock_ + 1},
        AssemblyMeshProfile{AssemblyMeshQuality::Normal}, semantic);
    if (!prepared) return false;
    window.installPreparedComponentProjection(std::move(prepared.projection));
    return true;
  }
};

}  // namespace solidar

namespace {
using namespace solidar;

std::unique_ptr<Document> boxDocument(double size, BodyId bodyId,
                                      FeatureId featureId) {
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      featureId,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(size, size, size).Shape()),
      "box"));
  CHECK(document->recompute());
  return document;
}

ProductDocument componentProduct() {
  ComponentDefinition root(DefinitionId{300},
                           boxDocument(10, BodyId{1}, FeatureId{11}));
  ComponentDefinition child(DefinitionId{200},
                            boxDocument(5, BodyId{1}, FeatureId{11}));
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, DefinitionId{200},
            RigidTransform3d::identity(), {}, "child")) ==
        ComponentOccurrenceInsertStatus::Inserted);
  std::vector<ComponentDefinition> definitions;
  definitions.push_back(std::move(child));
  definitions.push_back(std::move(root));
  auto made = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{300});
  CHECK(made);
  return std::move(*made.product);
}

ProductDocument assemblyProduct() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{20}, boxDocument(4, BodyId{2}, FeatureId{22}))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{100});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{1}, DefinitionId{20},
            RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions),
                                      DefinitionId{100});
  CHECK(made);
  return std::move(*made.product);
}

ProductDocumentIdentity componentIdentity() {
  return {ProjectId{{0x41, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
                     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x31}},
          {"component", "2026-10-31T00:00:00Z"}};
}

QModelIndex findOccurrence(ComponentTreeModel& model,
                           const QModelIndex& parent = {}) {
  for (int row = 0; row < model.rowCount(parent); ++row) {
    const QModelIndex index = model.index(row, 0, parent);
    const auto address = model.addressForIndex(index);
    if (address && std::holds_alternative<ComponentTreeOccurrenceAddress>(
                       address->get().value()))
      return index;
    const QModelIndex nested = findOccurrence(model, index);
    if (nested.isValid()) return nested;
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  constexpr AssemblyMeshProfile profile{AssemblyMeshQuality::Normal};

  {
    auto product = componentProduct();
    auto prepared = ComponentWorkspacePreparer::prepare(
        StagedApplicationDocument(std::move(product), std::nullopt,
                                  std::nullopt),
        ProductPresentationEpoch{41}, profile);
    CHECK(prepared);
    CHECK(prepared.workspace->epoch() == ProductPresentationEpoch{41});

    auto rejected = ComponentWorkspacePreparer::prepare(
        StagedApplicationDocument(Document{}, std::nullopt, std::nullopt),
        ProductPresentationEpoch{42}, profile);
    CHECK(!rejected);
    CHECK(rejected.status == ComponentWorkspacePrepareStatus::InvalidCarrier);

    rejected = ComponentWorkspacePreparer::prepare(
        StagedApplicationDocument(componentProduct(), std::nullopt,
                                  std::nullopt),
        ProductPresentationEpoch{42}, profile,
        ComponentViewportSessionFailurePoint::Resource);
    CHECK(!rejected);
    CHECK(rejected.status == ComponentWorkspacePrepareStatus::ResourceFailed);

    rejected = ComponentWorkspacePreparer::prepare(
        StagedApplicationDocument(componentProduct(), std::nullopt,
                                  std::nullopt),
        ProductPresentationEpoch{42}, profile,
        ComponentViewportSessionFailurePoint::None,
        ComponentTreeModelFailurePoint::ResourceFailure);
    CHECK(!rejected);
    CHECK(rejected.status == ComponentWorkspacePrepareStatus::ResourceFailed);

    // A projected-identity candidate can only be built for the exact live
    // carrier sealed by the transition. A different product with identical
    // authored IDs/content is still rejected by incarnation.
    auto sealedProduct = componentProduct();
    auto foreignProduct = componentProduct();
    auto transition = ComponentIdentityTransition::prepareFirstSave(
        sealedProduct, componentIdentity());
    CHECK(transition);
    const ComponentWorkspaceSemanticState semantic{
        {sealedProduct.rootDefinitionId(), std::nullopt}, std::nullopt,
        std::nullopt, {}};
    auto mismatched = ComponentProjectionPreparer::prepareProjectedIdentity(
        foreignProduct, ProductPresentationEpoch{43}, profile, semantic,
        std::nullopt, *transition.transition);
    CHECK(!mismatched);
    CHECK(mismatched.status == ComponentWorkspacePrepareStatus::InvalidCarrier);
    auto exact = ComponentProjectionPreparer::prepareProjectedIdentity(
        sealedProduct, ProductPresentationEpoch{43}, profile, semantic,
        std::nullopt, std::move(*transition.transition));
    CHECK(exact);
    CHECK(exact.projection->identityTransition());
    CHECK(exact.projection->identityTransition()->isCurrentSource(
        sealedProduct));
    const auto exactSnapshot =
        exact.projection->session()->publicationHandle().snapshotIfCurrent();
    CHECK(exactSnapshot);
    CHECK(exactSnapshot->treeProjection().productIdentity ==
          std::optional<ProductDocumentIdentity>{componentIdentity()});
  }

  QTemporaryDir temporary;
  CHECK(temporary.isValid());
  AppSettings settings(temporary.filePath(QStringLiteral("settings.ini")));
  MainWindow window(settings);
  CHECK(ComponentWorkspaceMainWindowTestAccess::installComponent(
      window, componentProduct()));
  auto* viewport = ComponentWorkspaceMainWindowTestAccess::viewport(window);
  auto* tree = ComponentWorkspaceMainWindowTestAccess::componentTree(window);
  auto* model = ComponentWorkspaceMainWindowTestAccess::componentModel(window);
  CHECK(viewport->componentSession());
  CHECK(viewport->componentSession()->scene()->instances().size() == 2);
  CHECK(ComponentWorkspaceMainWindowTestAccess::currentRibbon(window) ==
        ComponentWorkspaceMainWindowTestAccess::modelRibbon(window));
  CHECK(ComponentWorkspaceMainWindowTestAccess::currentRibbon(window) !=
        ComponentWorkspaceMainWindowTestAccess::assemblyRibbon(window));
  CHECK(ComponentWorkspaceMainWindowTestAccess::currentTree(window) == tree);
  CHECK(!window.findChild<QToolButton*>(QStringLiteral("createSketchCommand"))
             ->isEnabled());
  CHECK(window.findChild<QToolButton*>(QStringLiteral("displayModeCommand"))
            ->isEnabled());
  CHECK(!window.findChild<QToolButton*>(QStringLiteral("rulerCommand"))
             ->isEnabled());
  auto* quality =
      window.findChild<QToolButton*>(QStringLiteral("meshQualityCommand"));
  CHECK(quality && !quality->isEnabled());
  const QString componentQualityLabel = quality->text();
  QAction* createSketchOverflow = nullptr;
  QAction* qualityOverflow = nullptr;
  for (auto* action : window.findChildren<QAction*>()) {
    const QString source =
        action->property("sourceCommandObjectName").toString();
    if (source == QStringLiteral("createSketchCommand"))
      createSketchOverflow = action;
    if (source == QStringLiteral("meshQualityCommand"))
      qualityOverflow = action;
  }
  CHECK(createSketchOverflow && !createSketchOverflow->isEnabled());
  CHECK(qualityOverflow && !qualityOverflow->isEnabled());
  CHECK(!quality->menu()->actions().at(1)->isEnabled());
  quality->menu()->actions().at(1)->trigger();
  CHECK(quality->text() == componentQualityLabel);

  const QModelIndex occurrence = findOccurrence(*model);
  CHECK(occurrence.isValid());
  tree->setCurrentIndex(occurrence);
  CHECK(viewport->selectedComponentView());
  const auto selectedAddress = std::get<ComponentTreeOccurrenceAddress>(
      model->addressForIndex(occurrence)->get().value());
  CHECK(viewport->selectedComponentView()->component() ==
        selectedAddress.instance);

  // Replacing a publication must not call expandAll(): the user's expansion
  // choices survive while a successor tree is installed.
  const QModelIndex rootIndex = model->index(0, 0);
  CHECK(rootIndex.isValid());
  tree->setExpanded(rootIndex, true);
  tree->setExpanded(occurrence, false);
  CHECK(ComponentWorkspaceMainWindowTestAccess::rebuildComponentProjection(
      window));
  model = ComponentWorkspaceMainWindowTestAccess::componentModel(window);
  const QModelIndex rebuiltRoot = model->index(0, 0);
  const QModelIndex rebuiltOccurrence = findOccurrence(*model);
  CHECK(rebuiltRoot.isValid());
  CHECK(rebuiltOccurrence.isValid());
  CHECK(tree->isExpanded(rebuiltRoot));
  CHECK(!tree->isExpanded(rebuiltOccurrence));

  const auto rootInstance = viewport->componentSession()->components().front().address;
  CHECK(rootInstance.rootInstance());
  auto rootSelection = ComponentInstanceSelection::create(
      viewport->componentSession()->publicationHandle(), rootInstance);
  CHECK(rootSelection);
  CHECK(viewport->setSelectedComponent(rootSelection, true));
  const auto currentAddress = model->addressForIndex(tree->currentIndex());
  CHECK(currentAddress);
  CHECK(std::holds_alternative<ComponentTreeRootAddress>(
      currentAddress->get().value()));

  const auto incumbent = viewport->componentSession()->publicationHandle();
  ComponentWorkspaceMainWindowTestAccess::failComponentPreparation(window);
  CHECK(!ComponentWorkspaceMainWindowTestAccess::prepareComponent(
      window, componentProduct()));
  CHECK(viewport->componentSession());
  CHECK(viewport->componentSession()->publicationHandle()
            .isSameCurrentPublication(incumbent));
  ComponentWorkspaceMainWindowTestAccess::clearComponentPreparationFailure(
      window);

  ComponentWorkspaceMainWindowTestAccess::installPart(window);
  CHECK(!viewport->componentSession());
  CHECK(ComponentWorkspaceMainWindowTestAccess::currentTree(window) != tree);
  CHECK(quality->isEnabled());
  CHECK(createSketchOverflow->isEnabled());
  CHECK(qualityOverflow->isEnabled());
  CHECK(ComponentWorkspaceMainWindowTestAccess::installComponent(
      window, componentProduct()));
  CHECK(viewport->componentSession());
  CHECK(ComponentWorkspaceMainWindowTestAccess::installAssembly(
      window, assemblyProduct()));
  CHECK(!viewport->componentSession());
  CHECK(viewport->assemblySession());
  CHECK(ComponentWorkspaceMainWindowTestAccess::currentRibbon(window) ==
        ComponentWorkspaceMainWindowTestAccess::assemblyRibbon(window));

  return EXIT_SUCCESS;
}
