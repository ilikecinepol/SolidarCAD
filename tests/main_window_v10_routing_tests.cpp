#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTreeWidget>
#include <QToolButton>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "app/AppSettings.h"
#include "model/ImportedShapeFeature.h"
#include "model/internal/ComponentIdentityTransitionTestAccess.h"
#include "project/ProjectFile.h"
#include "project/ProductEnvelopeV10Decoder.h"
#include "project/ProductEnvelopeV11Decoder.h"
#include "project/internal/ProjectFormatV4DocumentEncoding.h"
#include "ui/ComponentTreeModel.h"
#include "ui/DrawingSheetView.h"
#include "ui/HistoryTimelineWidget.h"
#include "ui/MainWindow.h"
#include "ui/ModelRibbon.h"
#include "ui/Viewport.h"

namespace solidar {

class ComponentWorkspaceMainWindowTestAccess final {
 public:
  static DefinitionKind rootKind(const MainWindow& window) {
    return window.productDocument_.rootDefinitionKind();
  }
  static project::ProjectPersistenceState persistence(const MainWindow& window) {
    return window.projectPersistenceState_;
  }
  static const ProductDocumentIdentity* identity(const MainWindow& window) {
    return window.productDocument_.identity()
               ? &*window.productDocument_.identity()
               : nullptr;
  }
  static ProductIncarnationId incarnation(const MainWindow& window) {
    return window.productDocument_.incarnation();
  }
  static ModelRibbon* modelRibbon(MainWindow& window) {
    return window.modelRibbon_;
  }
  static QWidget* currentRibbon(MainWindow& window) {
    return window.ribbonStack_->currentWidget();
  }
  static QWidget* assemblyRibbon(MainWindow& window) {
    return window.assemblyRibbon_;
  }
  static ComponentTreeModel* componentTreeModel(MainWindow& window) {
    return window.componentTreeModel_;
  }
  static int drawingNodeCount(MainWindow& window) {
    if (!window.componentTreeModel_) return 0;
    int count = 0;
    const auto visit = [&](const auto& self, const QModelIndex& parent) -> void {
      for (int row = 0; row < window.componentTreeModel_->rowCount(parent);
           ++row) {
        const QModelIndex index =
            window.componentTreeModel_->index(row, 0, parent);
        const auto address = window.componentTreeModel_->addressForIndex(index);
        if (address && std::holds_alternative<ComponentTreeDrawingAddress>(
                           address->get().value()))
          ++count;
        self(self, index);
      }
    };
    visit(visit, {});
    return count;
  }
  static QTreeView* componentTree(MainWindow& window) {
    return window.componentTreeView_;
  }
  static DrawingSheetView* drawingSheet(MainWindow& window) {
    return window.drawingSheet_;
  }
  static QWidget* currentWorkspace(MainWindow& window) {
    return window.workspaceStack_->currentWidget();
  }
  static QModelIndex drawingIndex(MainWindow& window, DrawingId drawingId,
                                  bool rootInstance) {
    if (!window.componentTreeModel_ || !window.componentTreeView_) return {};
    const auto find = [&](const auto& self,
                          const QModelIndex& parent) -> QModelIndex {
      for (int row = 0; row < window.componentTreeModel_->rowCount(parent);
           ++row) {
        const QModelIndex index =
            window.componentTreeModel_->index(row, 0, parent);
        const auto address = window.componentTreeModel_->addressForIndex(index);
        if (address) {
          const auto* drawing = std::get_if<ComponentTreeDrawingAddress>(
              &address->get().value());
          if (drawing && drawing->drawingId == drawingId &&
              drawing->instance.rootInstance() == rootInstance) {
            return index;
          }
        }
        const QModelIndex nested = self(self, index);
        if (nested.isValid()) return nested;
      }
      return {};
    };
    return find(find, {});
  }
  static void doubleClickDrawing(MainWindow& window,
                                 const QModelIndex& index) {
    window.componentTreeView_->doubleClicked(index);
    QApplication::processEvents();
  }
  static bool openDrawing(MainWindow& window, DrawingId drawingId,
                          bool rootInstance) {
    const QModelIndex index = drawingIndex(window, drawingId, rootInstance);
    if (!index.isValid()) return false;
    doubleClickDrawing(window, index);
    return window.workspaceStack_->currentWidget() == window.drawingSheet_;
  }
  static void backFromDrawing(MainWindow& window) {
    auto* button = window.drawingSheet_->findChild<QPushButton*>(
        QStringLiteral("drawingBackToModelButton"));
    CHECK(button);
    button->click();
    QApplication::processEvents();
  }
  static Viewport* viewport(MainWindow& window) { return window.viewport_; }
  static bool saveTo(MainWindow& window, const QString& path, QString* error) {
    return window.saveProjectToPath(path, error);
  }
  static ProjectApplicationStatus saveAs(
      MainWindow& window,
      const MainWindow::SaveProjectAsTargetChooser& chooser,
      bool* attempted) {
    return window.runComponentSaveAsCommand(chooser, attempted);
  }
  static void failComponentPreparation(MainWindow& window, bool fail) {
    window.componentPreparationFailurePoint_ =
        fail ? ComponentViewportSessionFailurePoint::Resource
             : ComponentViewportSessionFailurePoint::None;
  }
  static bool installComponentProduct(MainWindow& window,
                                      ProductDocument product) {
    StagedApplicationDocument staged(
        std::move(product), std::nullopt,
        project::ProjectPersistenceState{
            project::ProjectPersistenceFormat::V10});
    auto prepared = window.prepareComponentWorkspace(std::move(staged));
    if (!prepared) return false;
    window.commitComponentReplacement(std::move(prepared), {});
    return true;
  }
  static ProductDocument& product(MainWindow& window) {
    return window.productDocument_;
  }
  static bool mutateRootComponentPlacementWithoutProjection(
      MainWindow& window) {
    class AcceptSuccessor final : public ProductDocumentSuccessor {
     public:
      ProductDocumentSuccessorStatus prepare(
          const ProductDocument&,
          ProductDocumentSuccessorImpact) noexcept override {
        return ProductDocumentSuccessorStatus::Prepared;
      }
    } accept;
    const auto* root = window.productDocument_.rootComponentDefinition();
    const auto* occurrence = root ? root->find(OccurrenceId{7}) : nullptr;
    auto path = OccurrencePath::create({OccurrenceId{7}});
    if (!root || !occurrence || !path) return false;
    const auto prepared = window.productDocument_.prepareComponentPlacement(
        {window.productDocument_.incarnation(),
         window.productDocument_.rootDefinitionId(), std::move(*path.path),
         occurrence->targetDefinitionId(), root->stateId(),
         occurrence->placement(), {1.0, 0.0, 0.0}});
    return prepared &&
           window.productDocument_.applyComponentPlacement(
               *prepared.transaction, accept) ==
               ComponentPlacementStatus::Committed;
  }
  static bool rebuildComponentProjection(MainWindow& window) {
    const ComponentWorkspaceInstanceKey root{
        window.productDocument_.rootDefinitionId(), std::nullopt};
    const ComponentWorkspaceSemanticState semantic{root, std::nullopt,
                                                   std::nullopt, {}};
    auto prepared = ComponentProjectionPreparer::prepare(
        window.productDocument_,
        ProductPresentationEpoch{window.componentPresentationEpochClock_ + 1},
        AssemblyMeshProfile{AssemblyMeshQuality::Normal}, semantic);
    if (!prepared) return false;
    window.installPreparedComponentProjection(std::move(prepared.projection));
    return true;
  }
  static std::unique_ptr<PreparedComponentProjection>
  prepareIdentityProjection(MainWindow& window,
                            ComponentIdentityTransition transition) {
    return window.prepareComponentIdentityProjection(std::move(transition));
  }
  static void installIdentityProjection(
      MainWindow& window,
      std::unique_ptr<PreparedComponentProjection> projection) noexcept {
    window.installComponentIdentityProjection(std::move(projection));
  }
  static bool installPartProduct(MainWindow& window, ProductDocument product,
                                 BodyId selectedBody) {
    window.viewport_->resetScene();
    window.productDocument_ = std::move(product);
    window.projectKind_ = EditorProjectKind::Part;
    window.projectPersistenceState_ = {
        project::ProjectPersistenceFormat::V5};
    window.modelHistory_.clear(true);
    window.rebuildFeatureTree();
    const ComponentWorkspaceInstanceKey root{
        window.productDocument_.rootDefinitionId(), std::nullopt};
    ComponentWorkspaceSemanticState semantic{
        root, root, ComponentWorkspaceBodyKey{root, selectedBody}, {}};
    auto prepared = PartWorkspaceProjectionPreparer::prepare(
        window.productDocument_, ProductPresentationEpoch{1},
        AssemblyMeshProfile{AssemblyMeshQuality::Normal}, semantic);
    if (!prepared) return false;
    window.installPreparedPartProjection(std::move(prepared.projection));
    window.applyProjectKind(EditorProjectKind::Part);
    return selectPartBody(window, selectedBody);
  }
  static bool selectPartBody(MainWindow& window, BodyId bodyId) {
    for (auto* item : window.featureTree_->findItems(
             QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive)) {
      if (item->data(0, Qt::UserRole).toInt() == 3 &&
          static_cast<BodyId>(item->data(0, Qt::UserRole + 2).toULongLong()) ==
              bodyId) {
        window.featureTree_->setCurrentItem(item);
        QApplication::processEvents();
        return true;
      }
    }
    return false;
  }
  static bool selectComponentBody(MainWindow& window, BodyId bodyId,
                                  bool rootBody) {
    if (!window.componentTreeModel_ || !window.componentTreeView_) return false;
    const auto find = [&](const auto& self,
                          const QModelIndex& parent) -> QModelIndex {
      for (int row = 0; row < window.componentTreeModel_->rowCount(parent);
           ++row) {
        const QModelIndex index =
            window.componentTreeModel_->index(row, 0, parent);
        const auto address =
            window.componentTreeModel_->addressForIndex(index);
        if (address) {
          const auto* body = std::get_if<ComponentTreeBodyAddress>(
              &address->get().value());
          if (body && body->body.bodyId == bodyId &&
              body->body.component.occurrencePath.has_value() != rootBody)
            return index;
        }
        const QModelIndex nested = self(self, index);
        if (nested.isValid()) return nested;
      }
      return {};
    };
    const QModelIndex index = find(find, {});
    if (!index.isValid()) return false;
    window.componentTreeView_->setCurrentIndex(index);
    QApplication::processEvents();
    return true;
  }
  static bool extractionEnabled(MainWindow& window) {
    auto* button = window.modelRibbon_->findChild<QToolButton*>(
        QStringLiteral("createComponentFromBodyCommand"));
    return button && button->isEnabled();
  }
  static bool extractionOverflowEnabled(MainWindow& window) {
    for (auto* action : window.modelRibbon_->findChildren<QAction*>())
      if (action->property("sourceCommandObjectName").toString() ==
          QStringLiteral("createComponentFromBodyCommand"))
        return action->isEnabled();
    return false;
  }
  static bool selectDescendantFromViewport(MainWindow& window) {
    auto* session = window.viewport_->componentSession();
    if (!session) return false;
    for (const auto& component : session->components()) {
      if (!component.address.occurrencePath) continue;
      const auto selection = ComponentInstanceSelection::create(
          session->publicationHandle(), component.address);
      if (!selection ||
          !window.viewport_->setSelectedComponent(selection, true))
        return false;
      QApplication::processEvents();
      return true;
    }
    return false;
  }
  static bool clearComponentFromViewport(MainWindow& window) {
    if (!window.viewport_->setSelectedComponent(std::nullopt, true))
      return false;
    QApplication::processEvents();
    return true;
  }
  static std::optional<ComponentPlacementDragRequest> placementRequest(
      MainWindow& window, std::array<double, 3> worldDeltaMm) {
    const auto* session = window.viewport_->componentSession();
    const auto& selected = window.viewport_->selectedComponentView();
    if (!session || !selected ||
        !selected->component().occurrencePath)
      return std::nullopt;
    return ComponentPlacementDragRequest{
        session->publicationHandle(), selected->component(), worldDeltaMm, 0};
  }
  static bool hoverSelectedComponent(MainWindow& window) {
    const auto* session = window.viewport_->componentSession();
    const auto& selected = window.viewport_->selectedComponentView();
    if (!session || !selected) return false;
    const auto hover = ComponentInstanceSelection::create(
        session->publicationHandle(), selected->component());
    return hover && window.viewport_->setHoveredComponent(hover, false);
  }
  static std::optional<ComponentInstanceAddress> hoveredComponent(
      const MainWindow& window) {
    const auto* session = window.viewport_->componentSession();
    if (!session || !session->hoveredComponent()) return std::nullopt;
    return session->hoveredComponent()->component();
  }
  static void requestPlacement(MainWindow& window,
                               ComponentPlacementDragRequest request) {
    window.viewport_->componentPlacementDragRequested(std::move(request));
    QApplication::processEvents();
  }
  static std::optional<RigidTransform3d> occurrencePlacement(
      const MainWindow& window, OccurrenceId occurrenceId) {
    const auto* root = window.productDocument_.rootComponentDefinition();
    const auto* occurrence = root ? root->find(occurrenceId) : nullptr;
    if (!occurrence) return std::nullopt;
    return occurrence->placement();
  }
  static QString extractionToolTip(MainWindow& window) {
    auto* button = window.modelRibbon_->findChild<QToolButton*>(
        QStringLiteral("createComponentFromBodyCommand"));
    return button ? button->toolTip() : QString{};
  }
  static void extract(MainWindow& window) {
    window.createComponentFromSelectedBody();
  }
  static void undo(MainWindow& window) { window.undoLastAction(); }
  static void redo(MainWindow& window) { window.redoLastAction(); }
  static std::size_t modelUndoCount(const MainWindow& window) {
    return window.modelHistory_.undoCount();
  }
  static std::size_t definitionCount(const MainWindow& window) {
    return window.productDocument_.definitions().size();
  }
  static const ComponentDefinition* rootComponent(const MainWindow& window) {
    return window.productDocument_.rootComponentDefinition();
  }
  static QModelIndex rootComponentIndex(MainWindow& window) {
    return window.componentTreeModel_
               ? window.componentTreeModel_->index(0, 0)
               : QModelIndex{};
  }
  static bool enterComponent(MainWindow& window, const QModelIndex& index) {
    window.enterComponentDefinitionEdit(index);
    QApplication::processEvents();
    return window.componentDefinitionEditContext_.has_value();
  }
  static void leaveComponent(MainWindow& window) {
    window.leaveComponentDefinitionEdit();
    QApplication::processEvents();
  }
  static bool componentEditActive(const MainWindow& window) {
    return window.componentDefinitionEditContext_.has_value();
  }
  static std::optional<DefinitionId> activeComponentDefinition(
      const MainWindow& window) {
    return window.componentDefinitionEditContext_
               ? std::optional<DefinitionId>{
                     window.componentDefinitionEditContext_->targetDefinitionId}
               : std::nullopt;
  }
  static bool activeComponentHasBody(const MainWindow& window, BodyId bodyId) {
    return window.componentDefinitionEditContext_ &&
           window.activePartDocument().findBody(bodyId);
  }
  static bool componentEditContextVisible(const MainWindow& window) {
    return window.viewport_->componentEditContextActive();
  }
  static bool componentEditContextGhosted(const MainWindow& window) {
    return window.viewport_->componentEditContextGhostsInactiveComponents();
  }
  static bool historyEnabled(const MainWindow& window) {
    return window.historyTimeline_ && window.historyTimeline_->isEnabled();
  }
  static bool createComponentVisible(MainWindow& window) {
    auto* command = window.modelRibbon_->findChild<QToolButton*>(
        QStringLiteral("createComponentFromBodyCommand"));
    return command && !command->isHidden();
  }
  static bool returnToComponentsVisible(MainWindow& window) {
    auto* command = window.modelRibbon_->findChild<QToolButton*>(
        QStringLiteral("returnToComponentsCommand"));
    return command && !command->isHidden() && command->isEnabled();
  }
  static bool definitionBackButtonVisible(const MainWindow& window) {
    return window.definitionEditBackButton_ &&
           !window.definitionEditBackButton_->isHidden();
  }
  static bool addActiveComponentBody(MainWindow& window, BodyId id) {
    if (!window.componentDefinitionEditContext_) return false;
    Document previous = window.activePartDocument();
    window.mutableStandaloneDocument().addBody(id, "edited component body");
    if (!window.mutableStandaloneDocument().recompute()) return false;
    return window.pushModelTransition(std::move(previous),
                                      window.captureHistorySelection());
  }
  static bool authoringEnabled(MainWindow& window) {
    auto* command = window.modelRibbon_->findChild<QToolButton*>(
        QStringLiteral("createSketchCommand"));
    return command && command->isEnabled();
  }
  static bool allAuthoringToolsEnabled(MainWindow& window) {
    static constexpr std::array<const char*, 16> commands{
        "createSketchCommand", "referenceImageCommand", "extrudeCommand",
        "pocketCommand",       "revolveCommand",        "filletCommand",
        "chamferCommand",      "joinBodiesCommand",     "moveCommand",
        "shellCommand",        "draftCommand",          "mirrorCommand",
        "linearPatternCommand", "circularPatternCommand", "rulerCommand",
        "meshQualityCommand"};
    return std::all_of(commands.begin(), commands.end(),
                       [&window](const char* objectName) {
                         auto* command =
                             window.modelRibbon_->findChild<QToolButton*>(
                                 QString::fromLatin1(objectName));
                         return command && command->isEnabled();
                       });
  }
  static bool componentTreeVisible(MainWindow& window) {
    return window.modelTreeStack_->currentWidget() ==
           window.componentTreeView_;
  }
  static bool recursiveComponentStructureVisible(MainWindow& window) {
    auto* model = window.componentTreeModel_;
    if (!model || model->rowCount() == 0) return false;
    const QModelIndex root = model->index(0, 0);
    QModelIndex component;
    for (int row = 0; row < model->rowCount(root); ++row) {
      const QModelIndex candidate = model->index(row, 0, root);
      const auto address = model->addressForIndex(candidate);
      if (address && std::holds_alternative<ComponentTreeOccurrenceAddress>(
                         address->get().value())) {
        component = candidate;
        break;
      }
    }
    if (!component.isValid() || component.parent() != root) return false;
    std::array<bool, 5> groups{};
    for (int row = 0; row < model->rowCount(component); ++row) {
      const QModelIndex child = model->index(row, 0, component);
      const auto address = model->addressForIndex(child);
      const auto* group =
          address ? std::get_if<ComponentTreeGroupAddress>(
                        &address->get().value())
                  : nullptr;
      if (group) groups[static_cast<std::size_t>(group->group)] = true;
    }
    return std::all_of(groups.begin(), groups.end(),
                       [](bool present) { return present; });
  }
  static QModelIndex componentIndex(
      MainWindow& window, DefinitionId definitionId,
      const std::optional<OccurrencePath>& path) {
    if (!window.componentTreeModel_) return {};
    const auto find = [&](const auto& self,
                          const QModelIndex& parent) -> QModelIndex {
      for (int row = 0; row < window.componentTreeModel_->rowCount(parent);
           ++row) {
        const QModelIndex index =
            window.componentTreeModel_->index(row, 0, parent);
        const auto address =
            window.componentTreeModel_->addressForIndex(index);
        if (address) {
          const auto* root = std::get_if<ComponentTreeRootAddress>(
              &address->get().value());
          const auto* occurrence =
              std::get_if<ComponentTreeOccurrenceAddress>(
                  &address->get().value());
          const ComponentInstanceAddress* instance =
              root ? &root->instance
                   : occurrence ? &occurrence->instance : nullptr;
          if (instance && instance->definitionId == definitionId &&
              instance->occurrencePath == path)
            return index;
        }
        const QModelIndex nested = self(self, index);
        if (nested.isValid()) return nested;
      }
      return {};
    };
    return find(find, {});
  }
  static bool componentMarkedActive(MainWindow& window,
                                    const QModelIndex& index) {
    return index.isValid() && window.componentTreeModel_ &&
           window.componentTreeModel_
               ->data(index, kComponentTreeActiveRole)
               .toBool() &&
           window.componentTreeModel_->data(index).toString().contains(
               QString::fromUtf8("активный"));
  }
  static bool componentTreeHasBody(MainWindow& window,
                                   const QModelIndex& component,
                                   BodyId bodyId) {
    if (!component.isValid() || !window.componentTreeModel_) return false;
    for (int row = 0;
         row < window.componentTreeModel_->rowCount(component); ++row) {
      const QModelIndex group =
          window.componentTreeModel_->index(row, 0, component);
      const auto groupAddress =
          window.componentTreeModel_->addressForIndex(group);
      const auto* typedGroup =
          groupAddress ? std::get_if<ComponentTreeGroupAddress>(
                             &groupAddress->get().value())
                       : nullptr;
      if (!typedGroup || typedGroup->group != ComponentTreeGroupKind::Bodies)
        continue;
      for (int bodyRow = 0;
           bodyRow < window.componentTreeModel_->rowCount(group); ++bodyRow) {
        const QModelIndex body =
            window.componentTreeModel_->index(bodyRow, 0, group);
        const auto address =
            window.componentTreeModel_->addressForIndex(body);
        const auto* typedBody =
            address ? std::get_if<ComponentTreeBodyAddress>(
                          &address->get().value())
                    : nullptr;
        if (typedBody && typedBody->body.bodyId == bodyId) return true;
      }
    }
    return false;
  }
  static void failExtractionProjection(MainWindow& window, bool fail) {
    window.componentExtractionProjectionFailurePoint_ =
        fail ? ModelWorkspaceProjectionFailurePoint::ComponentSession
             : ModelWorkspaceProjectionFailurePoint::None;
  }
};

}  // namespace solidar

namespace {

using namespace solidar;
using Access = ComponentWorkspaceMainWindowTestAccess;

class SharedSketchBoxFeature final : public ShapeFeature {
 public:
  SharedSketchBoxFeature(FeatureId id, SketchId sketchId, std::string name)
      : ShapeFeature(id, std::move(name)), sketchId_(sketchId) {}
  FeatureDependencies dependencies() const override {
    FeatureDependencies result;
    result.sketchIds.push_back(sketchId_);
    return result;
  }
  std::unique_ptr<Feature> clone() const override {
    return std::make_unique<SharedSketchBoxFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    clearShape();
    setShape(std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(8.0, 6.0, 4.0).Shape()));
    markValid();
    return true;
  }

 private:
  SketchId sketchId_;
};

constexpr ProjectId kProject{
    {0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x10}};
constexpr ProjectId kAssemblyProject{
    {0x22, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x20}};
constexpr ProjectId kTailProject{
    {0x32, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x42, 0xd3,
     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x30}};

bool samePlacement(const SketchPlacement& left,
                   const SketchPlacement& right) {
  return left.origin.x == right.origin.x &&
         left.origin.y == right.origin.y &&
         left.origin.z == right.origin.z &&
         left.xDirection.x == right.xDirection.x &&
         left.xDirection.y == right.xDirection.y &&
         left.xDirection.z == right.xDirection.z &&
         left.yDirection.x == right.yDirection.x &&
         left.yDirection.y == right.yDirection.y &&
         left.yDirection.z == right.yDirection.z;
}

ProductDocument twoBodyPartProduct() {
  auto document = std::make_unique<Document>();
  auto& first = document->addBody(BodyId{201}, "first");
  first.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{301},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(8.0, 6.0, 4.0).Shape()),
      "first feature"));
  auto& second = document->addBody(BodyId{202}, "second");
  second.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{302},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(5.0, 4.0, 3.0).Shape()),
      "second feature"));
  CHECK(document->recompute());
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{1}, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(created);
  return std::move(*created.product);
}

ProductDocument faceSupportedPartProduct() {
  auto document = std::make_unique<Document>();
  auto& support = document->addBody(BodyId{10}, "support");
  support.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{20},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(8.0, 6.0, 4.0).Shape()),
      "support feature"));
  auto& sketch = document->addSketch(SketchId{30}, "face supported");
  sketch.geometry.addRectangle({0.0, 0.0}, {5.0, 5.0});
  sketch.support =
      {SketchSupportType::Face,
       FaceReference{support.id(), FeatureId{20}, 0}};
  document->addBody(BodyId{11}, "dependent")
      .addFeature(std::make_unique<SharedSketchBoxFeature>(
          FeatureId{21}, sketch.id, "dependent feature"));
  CHECK(document->recomputeDetailed().success);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{1}, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(created);
  return std::move(*created.product);
}

ProductDocument sharedSketchPartProduct() {
  auto document = std::make_unique<Document>();
  auto& sketch = document->addSketch(SketchId{1}, "shared");
  sketch.geometry.addRectangle({0.0, 0.0}, {5.0, 5.0});
  document->addBody(BodyId{10}, "a")
      .addFeature(std::make_unique<SharedSketchBoxFeature>(
          FeatureId{20}, sketch.id, "a"));
  document->addBody(BodyId{11}, "b")
      .addFeature(std::make_unique<SharedSketchBoxFeature>(
          FeatureId{21}, sketch.id, "b"));
  CHECK(document->recompute());
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(DefinitionId{1}, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  auto created =
      ProductDocument::create(std::move(definitions), DefinitionId{1});
  CHECK(created);
  return std::move(*created.product);
}

void writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  CHECK(file.write(bytes) == bytes.size());
  file.close();
}

QByteArray readBytes(const QString& path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

std::unique_ptr<Document> boxDocument(BodyId bodyId, FeatureId featureId,
                                      double size) {
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      featureId,
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(size, size, size).Shape()),
      "box"));
  CHECK(document->recomputeDetailed().success);
  return document;
}

QJsonObject v4Payload(const Document& document, const QString& name) {
  const auto encoded = project::detail::encodeProjectFormatV4DocumentRoot(
      document, {name, QStringLiteral("2026-10-31T00:00:00Z")});
  CHECK(encoded.succeeded());
  return *encoded.root;
}

QByteArray handAuthoredNestedV10() {
  auto child = boxDocument(BodyId{101}, FeatureId{201}, 4.0);
  auto root = boxDocument(BodyId{102}, FeatureId{202}, 8.0);
  QJsonObject childDefinition{
      {QStringLiteral("definitionId"), 1},
      {QStringLiteral("definitionKind"), QStringLiteral("component")},
      {QStringLiteral("payload"),
       QJsonObject{{QStringLiteral("payloadKind"),
                    QStringLiteral("embedded-component-v4")},
                   {QStringLiteral("data"),
                    v4Payload(*child, QStringLiteral("component UI"))}}},
      {QStringLiteral("occurrences"), QJsonArray{}}};
  QJsonObject occurrence{
      {QStringLiteral("occurrenceId"), 7},
      {QStringLiteral("targetDefinitionId"), 1},
      {QStringLiteral("placement"),
       QJsonObject{{QStringLiteral("translationMm"), QJsonArray{12, 0, 0}},
                   {QStringLiteral("quaternionWxyz"),
                    QJsonArray{1, 0, 0, 0}}}},
      {QStringLiteral("name"), QStringLiteral("child")},
      {QStringLiteral("visible"), true},
      {QStringLiteral("suppressed"), false},
      {QStringLiteral("grounded"), false}};
  QJsonObject rootDefinition{
      {QStringLiteral("definitionId"), 2},
      {QStringLiteral("definitionKind"), QStringLiteral("component")},
      {QStringLiteral("payload"),
       QJsonObject{{QStringLiteral("payloadKind"),
                    QStringLiteral("embedded-component-v4")},
                   {QStringLiteral("data"),
                    v4Payload(*root, QStringLiteral("component UI"))}}},
      {QStringLiteral("occurrences"), QJsonArray{occurrence}}};
  QJsonObject envelope{
      {QStringLiteral("format"), QStringLiteral("solidar-project")},
      {QStringLiteral("version"), 10},
      {QStringLiteral("capabilities"),
       QJsonArray{QStringLiteral("component-definitions-v1"),
                  QStringLiteral("nested-components-v1"),
                  QStringLiteral("product-envelope-v1")}},
      {QStringLiteral("projectId"),
       QStringLiteral("123e4567-e89b-42d3-a456-426614174010")},
      {QStringLiteral("rootDefinitionId"), 2},
      {QStringLiteral("metadata"),
       QJsonObject{{QStringLiteral("name"), QStringLiteral("component UI")},
                   {QStringLiteral("createdAt"),
                    QStringLiteral("2026-10-31T00:00:00Z")}}},
      {QStringLiteral("definitions"),
       QJsonArray{childDefinition, rootDefinition}}};
  const QByteArray bytes =
      QJsonDocument(envelope).toJson(QJsonDocument::Indented);
  CHECK(project::scanProductEnvelopeV10(QByteArrayView(bytes)).hasCandidate());
  return bytes;
}

QByteArray handAuthoredNestedV11() {
  QJsonObject envelope =
      QJsonDocument::fromJson(handAuthoredNestedV10()).object();
  envelope[QStringLiteral("version")] = 11;
  envelope[QStringLiteral("capabilities")] =
      QJsonArray{QStringLiteral("component-definitions-v1"),
                 QStringLiteral("component-drawings-v1"),
                 QStringLiteral("nested-components-v1"),
                 QStringLiteral("product-envelope-v1")};
  auto definitions = envelope.value(QStringLiteral("definitions")).toArray();
  auto child = definitions[0].toObject();
  child[QStringLiteral("drawings")] = QJsonArray{QJsonObject{
      {QStringLiteral("drawingId"), 1},
      {QStringLiteral("name"), QStringLiteral("Child drawing")},
      {QStringLiteral("paper"), QStringLiteral("A4")},
      {QStringLiteral("orientation"), QStringLiteral("portrait")},
      {QStringLiteral("scale"), 1.0},
      {QStringLiteral("sourceBodyIds"), QJsonArray{101}}}};
  definitions[0] = child;
  auto root = definitions[1].toObject();
  auto rootDocument = boxDocument(BodyId{102}, FeatureId{202}, 8.0);
  auto& secondRootBody = rootDocument->addBody(BodyId{103}, "second root");
  secondRootBody.addFeature(std::make_unique<ImportedShapeFeature>(
      FeatureId{203},
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(3.0, 4.0, 5.0).Shape()),
      "second root box"));
  CHECK(rootDocument->recomputeDetailed().success);
  auto rootPayload = root.value(QStringLiteral("payload")).toObject();
  rootPayload[QStringLiteral("data")] =
      v4Payload(*rootDocument, QStringLiteral("component UI"));
  root[QStringLiteral("payload")] = rootPayload;
  root[QStringLiteral("drawings")] = QJsonArray{QJsonObject{
      {QStringLiteral("drawingId"), 2},
      {QStringLiteral("name"), QStringLiteral("Root drawing")},
      {QStringLiteral("paper"), QStringLiteral("A4")},
      {QStringLiteral("orientation"), QStringLiteral("landscape")},
      {QStringLiteral("scale"), 2.0},
      {QStringLiteral("sourceBodyIds"), QJsonArray{}}}};
  definitions[1] = root;
  envelope[QStringLiteral("definitions")] = definitions;
  const QByteArray bytes =
      QJsonDocument(envelope).toJson(QJsonDocument::Indented);
  CHECK(project::scanProductEnvelopeV11(QByteArrayView(bytes)).hasCandidate());
  return bytes;
}

std::unique_ptr<ProductDocument> assemblyProduct() {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            DefinitionId{1}, boxDocument(BodyId{11}, FeatureId{21}, 5.0))) ==
        DefinitionInsertStatus::Inserted);
  AssemblyDefinition root(DefinitionId{2});
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{3}, DefinitionId{1}, RigidTransform3d::identity(),
            {}, "part")) == AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(
      std::move(definitions), DefinitionId{2},
      ProductDocumentIdentity{kAssemblyProject,
                              {"assembly", "2026-10-31T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<ProductDocument> unidentifiedComponentProduct() {
  std::vector<ComponentDefinition> definitions;
  definitions.emplace_back(DefinitionId{51},
                           boxDocument(BodyId{61}, FeatureId{71}, 6.0));
  auto created = ProductDocument::createComponentGraph(
      std::move(definitions), DefinitionId{51}, std::nullopt);
  CHECK(created);
  CHECK(!created.product->identity());
  return std::move(created.product);
}

ComponentWorkspaceInstanceKey semanticKey(
    const ComponentInstanceAddress& address) {
  return {address.definitionId, address.occurrencePath};
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("SolidarCAD-tests"));
  QCoreApplication::setApplicationName(
      QStringLiteral("main-window-v10-routing-tests"));
  AppSettings settings;
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("main-window-v10-XXXXXX")));
  CHECK(directory.isValid());

  const QString partPath = directory.filePath(QStringLiteral("part.solidar"));
  CHECK(project::ProjectFile::create(partPath));
  const QString componentPath =
      directory.filePath(QStringLiteral("component.solidar"));
  writeBytes(componentPath, handAuthoredNestedV10());
  const QString componentV11Path =
      directory.filePath(QStringLiteral("component-v11.solidar"));
  writeBytes(componentV11Path, handAuthoredNestedV11());
  auto assembly = assemblyProduct();
  const QString assemblyPath =
      directory.filePath(QStringLiteral("assembly.solidar"));
  ProjectApplicationService service;
  CHECK(service
            .saveAssembly(assemblyPath, *assembly,
                          {project::ProjectPersistenceFormat::V7})
            .succeeded());

  MainWindow window(settings);
  QString error;
  CHECK(window.loadProject(componentPath, &error));
  CHECK(error.isEmpty());
  CHECK(Access::rootKind(window) == DefinitionKind::Component);
  CHECK(Access::persistence(window) ==
        project::ProjectPersistenceState{project::ProjectPersistenceFormat::V10});
  CHECK(Access::currentRibbon(window) == Access::modelRibbon(window));
  CHECK(Access::currentRibbon(window) != Access::assemblyRibbon(window));
  CHECK(Access::componentTreeModel(window));
  CHECK(Access::componentTree(window)->model() ==
        Access::componentTreeModel(window));
  CHECK(Access::viewport(window)->componentSession());
  CHECK(Access::viewport(window)->componentSession()->scene());
  CHECK(Access::viewport(window)->componentSession()->scene()->instances().size() ==
        2);

  const QModelIndex activeRoot = Access::rootComponentIndex(window);
  CHECK(activeRoot.isValid());
  CHECK(Access::componentTreeModel(window)->data(activeRoot).toString() ==
        QStringLiteral("component"));
  CHECK(Access::componentTreeModel(window)
            ->data(activeRoot, Qt::CheckStateRole)
            .toInt() == Qt::Checked);
  bool directChildComponent = false;
  QModelIndex directChildIndex;
  OccurrenceId directChildId;
  DefinitionId directChildDefinitionId;
  std::optional<OccurrencePath> directChildPath;
  for (int row = 0;
       row < Access::componentTreeModel(window)->rowCount(activeRoot); ++row) {
    const QModelIndex child =
        Access::componentTreeModel(window)->index(row, 0, activeRoot);
    const auto address =
        Access::componentTreeModel(window)->addressForIndex(child);
    if (address) {
      if (const auto* occurrence =
              std::get_if<ComponentTreeOccurrenceAddress>(
                  &address->get().value())) {
        directChildComponent = true;
        directChildIndex = child;
        directChildId = occurrence->instance.occurrencePath->entries().back();
        directChildDefinitionId = occurrence->instance.definitionId;
        directChildPath = occurrence->instance.occurrencePath;
      }
    }
  }
  CHECK(directChildComponent);
  CHECK(Access::componentTreeModel(window)->setData(
      activeRoot, Qt::Unchecked, Qt::CheckStateRole));
  QApplication::processEvents();
  CHECK(!Access::viewport(window)->componentRootVisible());
  CHECK(Access::componentTreeModel(window)->setData(
      activeRoot, Qt::Checked, Qt::CheckStateRole));
  QApplication::processEvents();
  CHECK(Access::viewport(window)->componentRootVisible());
  CHECK(Access::componentTreeModel(window)->setData(
      directChildIndex, Qt::Unchecked, Qt::CheckStateRole));
  QApplication::processEvents();
  CHECK(!Access::rootComponent(window)->find(directChildId)->flags().visible);

  const QModelIndex activeRootAfterVisibility =
      Access::rootComponentIndex(window);
  QModelIndex hiddenChild;
  for (int row = 0; row < Access::componentTreeModel(window)->rowCount(
                              activeRootAfterVisibility);
       ++row) {
    const QModelIndex child = Access::componentTreeModel(window)->index(
        row, 0, activeRootAfterVisibility);
    const auto address =
        Access::componentTreeModel(window)->addressForIndex(child);
    const auto* occurrence = address
                                 ? std::get_if<ComponentTreeOccurrenceAddress>(
                                       &address->get().value())
                                 : nullptr;
    if (occurrence && occurrence->instance.occurrencePath->entries().back() ==
                          directChildId) {
      hiddenChild = child;
      break;
    }
  }
  CHECK(hiddenChild.isValid());
  CHECK(Access::componentTreeModel(window)->setData(
      hiddenChild, Qt::Checked, Qt::CheckStateRole));
  QApplication::processEvents();
  CHECK(Access::rootComponent(window)->find(directChildId)->flags().visible);

  const QModelIndex rootBeforeRename = Access::rootComponentIndex(window);
  QModelIndex childToRename;
  for (int row = 0; row < Access::componentTreeModel(window)->rowCount(
                              rootBeforeRename);
       ++row) {
    const QModelIndex child = Access::componentTreeModel(window)->index(
        row, 0, rootBeforeRename);
    const auto address =
        Access::componentTreeModel(window)->addressForIndex(child);
    const auto* occurrence = address
                                 ? std::get_if<ComponentTreeOccurrenceAddress>(
                                       &address->get().value())
                                 : nullptr;
    if (occurrence && occurrence->instance.occurrencePath->entries().back() ==
                          directChildId) {
      childToRename = child;
      break;
    }
  }
  CHECK(childToRename.isValid());
  CHECK(Access::componentTreeModel(window)->setData(
      childToRename, QString::fromUtf8("Переименованный компонент"),
      Qt::EditRole));
  QApplication::processEvents();
  CHECK(Access::rootComponent(window)->find(directChildId)->displayNameUtf8() ==
        "Переименованный компонент");
  const QModelIndex rootAfterRename = Access::rootComponentIndex(window);
  bool renamedVisibleInTree = false;
  for (int row = 0; row < Access::componentTreeModel(window)->rowCount(
                              rootAfterRename);
       ++row) {
    const QModelIndex child = Access::componentTreeModel(window)->index(
        row, 0, rootAfterRename);
    const auto address =
        Access::componentTreeModel(window)->addressForIndex(child);
    const auto* occurrence = address
                                 ? std::get_if<ComponentTreeOccurrenceAddress>(
                                       &address->get().value())
                                 : nullptr;
    if (occurrence && occurrence->instance.occurrencePath->entries().back() ==
                          directChildId) {
      renamedVisibleInTree =
          Access::componentTreeModel(window)
              ->data(child, Qt::DisplayRole)
              .toString()
              .startsWith(QString::fromUtf8("Переименованный компонент"));
      break;
    }
  }
  CHECK(renamedVisibleInTree);

  CHECK(Access::enterComponent(window, Access::rootComponentIndex(window)));
  CHECK(Access::componentEditActive(window));
  CHECK(Access::componentEditContextVisible(window));
  CHECK(!Access::componentEditContextGhosted(window));
  CHECK(!Access::returnToComponentsVisible(window));
  CHECK(!Access::definitionBackButtonVisible(window));
  CHECK(Access::authoringEnabled(window));
  CHECK(Access::allAuthoringToolsEnabled(window));
  CHECK(Access::historyEnabled(window));
  CHECK(Access::createComponentVisible(window));
  CHECK(Access::componentTreeVisible(window));
  CHECK(Access::recursiveComponentStructureVisible(window));
  CHECK(Access::viewport(window)->componentSession());
  CHECK(Access::componentMarkedActive(
      window,
      Access::componentIndex(window,
                             Access::product(window).rootDefinitionId(),
                             std::nullopt)));
  CHECK(Access::addActiveComponentBody(window, BodyId{901}));
  CHECK(Access::product(window)
            .componentDocument(Access::product(window).rootDefinitionId())
            ->findBody(BodyId{901}));
  CHECK(Access::componentTreeHasBody(
      window,
      Access::componentIndex(window,
                             Access::product(window).rootDefinitionId(),
                             std::nullopt),
      BodyId{901}));
  CHECK(Access::selectDescendantFromViewport(window));
  const auto rootAuthoringPlacementBefore =
      Access::occurrencePlacement(window, directChildId);
  CHECK(rootAuthoringPlacementBefore);
  const auto rootAuthoringPlacement = Access::placementRequest(
      window, std::array<double, 3>{12.0, 0.0, 0.0});
  CHECK(rootAuthoringPlacement);
  Access::requestPlacement(window, *rootAuthoringPlacement);
  const auto rootAuthoringPlacementAfter =
      Access::occurrencePlacement(window, directChildId);
  CHECK(rootAuthoringPlacementAfter);
  auto expectedRootAuthoringTranslation =
      rootAuthoringPlacementBefore->translationMm();
  expectedRootAuthoringTranslation[0] += 12.0;
  CHECK(rootAuthoringPlacementAfter->translationMm() ==
        expectedRootAuthoringTranslation);
  CHECK(Access::componentEditActive(window));
  CHECK(Access::viewport(window)->componentSession());
  CHECK(Access::authoringEnabled(window));
  CHECK(Access::modelUndoCount(window) == 2);
  Access::leaveComponent(window);
  CHECK(!Access::componentEditActive(window));
  CHECK(Access::viewport(window)->componentSession());
  const QModelIndex reopenedActiveRoot = Access::rootComponentIndex(window);
  CHECK(Access::enterComponent(window, reopenedActiveRoot));
  CHECK(Access::modelUndoCount(window) == 2);
  Access::leaveComponent(window);
  const QModelIndex activeChild = Access::componentIndex(
      window, directChildDefinitionId, directChildPath);
  CHECK(activeChild.isValid());
  CHECK(Access::enterComponent(window, activeChild));
  CHECK(!Access::returnToComponentsVisible(window));
  CHECK(!Access::definitionBackButtonVisible(window));
  CHECK(Access::componentMarkedActive(
      window,
      Access::componentIndex(window, directChildDefinitionId,
                             directChildPath)));
  CHECK(Access::addActiveComponentBody(window, BodyId{902}));
  CHECK(Access::product(window)
            .componentDocument(directChildDefinitionId)
            ->findBody(BodyId{902}));
  CHECK(!Access::product(window)
             .componentDocument(Access::product(window).rootDefinitionId())
             ->findBody(BodyId{902}));
  CHECK(Access::componentTreeHasBody(
      window,
      Access::componentIndex(window, directChildDefinitionId,
                             directChildPath),
      BodyId{902}));
  CHECK(Access::enterComponent(window, Access::rootComponentIndex(window)));
  CHECK(Access::componentEditActive(window));
  CHECK(Access::activeComponentDefinition(window) ==
        std::optional<DefinitionId>{
            Access::product(window).rootDefinitionId()});
  CHECK(Access::viewport(window)->componentSession());
  CHECK(Access::authoringEnabled(window));
  Access::leaveComponent(window);
  CHECK(Access::clearComponentFromViewport(window));

  // V11 uses the same off-side ComponentWorkspace and exposes persistent
  // drawing nodes without activating a drawing editor. Saving retains V11.
  MainWindow v11Window(settings);
  CHECK(v11Window.loadProject(componentV11Path, &error));
  CHECK(Access::rootKind(v11Window) == DefinitionKind::Component);
  CHECK(Access::persistence(v11Window) ==
        project::ProjectPersistenceState{
            project::ProjectPersistenceFormat::V11});
  CHECK(Access::currentRibbon(v11Window) == Access::modelRibbon(v11Window));
  CHECK(Access::drawingNodeCount(v11Window) == 2);
  CHECK(Access::openDrawing(v11Window, DrawingId{1}, false));
  CHECK(Access::drawingSheet(v11Window)->sourceTitle() ==
        QStringLiteral("Child drawing"));
  CHECK(Access::drawingSheet(v11Window)->sourceBodyCount() == 1);
  CHECK(Access::drawingSheet(v11Window)->sourceSolidCount() == 1);
  CHECK(Access::currentRibbon(v11Window) == Access::modelRibbon(v11Window));
  CHECK(Access::currentRibbon(v11Window) != Access::assemblyRibbon(v11Window));
  Access::backFromDrawing(v11Window);
  CHECK(Access::currentWorkspace(v11Window) == Access::viewport(v11Window));
  CHECK(Access::openDrawing(v11Window, DrawingId{2}, true));
  CHECK(Access::drawingSheet(v11Window)->sourceTitle() ==
        QStringLiteral("Root drawing"));
  CHECK(Access::drawingSheet(v11Window)->sourceBodyCount() == 2);
  CHECK(Access::drawingSheet(v11Window)->sourceSolidCount() == 2);
  Access::backFromDrawing(v11Window);
  CHECK(Access::selectDescendantFromViewport(v11Window));
  const auto selectedV11 = Access::viewport(v11Window)
                               ->componentSession()
                               ->selectedComponent()
                               ->component();
  CHECK(Access::saveTo(v11Window, componentV11Path, &error));
  CHECK(Access::currentWorkspace(v11Window) == Access::viewport(v11Window));
  CHECK(!Access::drawingSheet(v11Window)->sourceShape());
  CHECK(Access::drawingSheet(v11Window)->sourceTitle().isEmpty());
  CHECK(project::scanProductEnvelopeV11(
            QByteArrayView(readBytes(componentV11Path)))
            .hasCandidate());
  CHECK(Access::persistence(v11Window).format ==
        project::ProjectPersistenceFormat::V11);
  CHECK(Access::viewport(v11Window)->componentSession()->selectedComponent());
  CHECK(Access::viewport(v11Window)
            ->componentSession()
            ->selectedComponent()
            ->component() == selectedV11);
  CHECK(!v11Window.isWindowModified());
  CHECK(Access::openDrawing(v11Window, DrawingId{1}, false));
  CHECK(Access::drawingSheet(v11Window)->sourceShape());
  CHECK(v11Window.loadProject(partPath, &error));
  CHECK(Access::rootKind(v11Window) == DefinitionKind::Part);
  CHECK(Access::currentWorkspace(v11Window) == Access::viewport(v11Window));
  CHECK(!Access::drawingSheet(v11Window)->sourceShape());
  CHECK(Access::drawingSheet(v11Window)->sourceTitle().isEmpty());

  // A model mutation that advances a definition state without rebuilding the
  // immutable tree publication makes its drawing nodes stale even when the
  // drawing metadata itself is byte-for-byte unchanged. Rejection preserves
  // the incumbent sheet; a fresh exact projection makes activation valid.
  MainWindow staleDrawingWindow(settings);
  CHECK(staleDrawingWindow.loadProject(componentV11Path, &error));
  CHECK(Access::openDrawing(staleDrawingWindow, DrawingId{1}, false));
  const QModelIndex staleRootDrawing =
      Access::drawingIndex(staleDrawingWindow, DrawingId{2}, true);
  CHECK(staleRootDrawing.isValid());
  const TopoDS_Shape* incumbentShape =
      Access::drawingSheet(staleDrawingWindow)->sourceShape();
  const QString incumbentTitle =
      Access::drawingSheet(staleDrawingWindow)->sourceTitle();
  const std::size_t incumbentBodies =
      Access::drawingSheet(staleDrawingWindow)->sourceBodyCount();
  CHECK(Access::mutateRootComponentPlacementWithoutProjection(
      staleDrawingWindow));
  Access::doubleClickDrawing(staleDrawingWindow, staleRootDrawing);
  CHECK(Access::currentWorkspace(staleDrawingWindow) ==
        Access::drawingSheet(staleDrawingWindow));
  CHECK(Access::drawingSheet(staleDrawingWindow)->sourceShape() ==
        incumbentShape);
  CHECK(Access::drawingSheet(staleDrawingWindow)->sourceTitle() ==
        incumbentTitle);
  CHECK(Access::drawingSheet(staleDrawingWindow)->sourceBodyCount() ==
        incumbentBodies);
  CHECK(Access::rebuildComponentProjection(staleDrawingWindow));
  CHECK(Access::openDrawing(staleDrawingWindow, DrawingId{2}, true));
  CHECK(Access::drawingSheet(staleDrawingWindow)->sourceTitle() ==
        QStringLiteral("Root drawing"));
  CHECK(Access::drawingSheet(staleDrawingWindow)->sourceBodyCount() == 2);

  // First Save prepares the exact post-identity publication before writing.
  // A preparation failure therefore creates no destination and mutates no
  // product/UI state; success retires the old handle and restores interaction.
  MainWindow firstSaveWindow(settings);
  auto unidentified = unidentifiedComponentProduct();
  CHECK(Access::installComponentProduct(firstSaveWindow,
                                        std::move(*unidentified)));
  auto* firstSession = Access::viewport(firstSaveWindow)->componentSession();
  CHECK(firstSession && !firstSession->components().empty());
  const auto firstAddress = firstSession->components().front().address;
  const auto firstSelection = ComponentInstanceSelection::create(
      firstSession->publicationHandle(), firstAddress);
  CHECK(firstSelection);
  CHECK(Access::viewport(firstSaveWindow)
            ->setSelectedComponent(firstSelection, false));
  CHECK(Access::viewport(firstSaveWindow)
            ->setHoveredComponent(firstSelection, false));
  const auto firstOldPublication = firstSession->publicationHandle();
  const QString firstSavePath =
      directory.filePath(QStringLiteral("first-save.solidar"));
  Access::failComponentPreparation(firstSaveWindow, true);
  CHECK(!Access::saveTo(firstSaveWindow, firstSavePath, &error));
  Access::failComponentPreparation(firstSaveWindow, false);
  CHECK(!QFileInfo::exists(firstSavePath));
  CHECK(!Access::identity(firstSaveWindow));
  CHECK(firstOldPublication.isSameCurrentPublication(
      Access::viewport(firstSaveWindow)
          ->componentSession()
          ->publicationHandle()));
  CHECK(Access::saveTo(firstSaveWindow, firstSavePath, &error));
  CHECK(Access::identity(firstSaveWindow));
  CHECK(!firstOldPublication.isCurrent());
  const auto* firstSavedSession =
      Access::viewport(firstSaveWindow)->componentSession();
  CHECK(firstSavedSession && firstSavedSession->selectedComponent());
  CHECK(firstSavedSession->hoveredComponent());
  CHECK(semanticKey(firstSavedSession->selectedComponent()->component()) ==
        semanticKey(firstAddress));
  CHECK(semanticKey(firstSavedSession->hoveredComponent()->component()) ==
        semanticKey(firstAddress));
  const auto firstSnapshot =
      firstSavedSession->publicationHandle().snapshotIfCurrent();
  CHECK(firstSnapshot);
  CHECK(firstSnapshot->treeProjection().productIdentity ==
        std::optional<ProductDocumentIdentity>{
            *Access::identity(firstSaveWindow)});
  const QModelIndex firstRootIndex =
      Access::componentTreeModel(firstSaveWindow)->index(0, 0);
  const auto firstRootTreeAddress =
      Access::componentTreeModel(firstSaveWindow)
          ->addressForIndex(firstRootIndex);
  CHECK(firstRootTreeAddress);
  CHECK(firstRootTreeAddress->get().publication().isSameCurrentPublication(
      firstSavedSession->publicationHandle()));
  CHECK(Access::componentTree(firstSaveWindow)->currentIndex().isValid());

  // Save reaches a byte-stable fixed point and reopening installs another
  // complete component workspace rather than the legacy assembly branch.
  CHECK(Access::saveTo(window, componentPath, &error));
  const QByteArray fixed = readBytes(componentPath);
  CHECK(Access::saveTo(window, componentPath, &error));
  CHECK(readBytes(componentPath) == fixed);
  MainWindow reopened(settings);
  CHECK(reopened.loadProject(componentPath, &error));
  CHECK(Access::rootKind(reopened) == DefinitionKind::Component);

  // The critical post-writer tail consumes an already sealed projection. It
  // must not re-enter Product definition-state validation; all inspection of
  // the installed publication happens only after the tail has completed.
  MainWindow tailWindow(settings);
  CHECK(tailWindow.loadProject(componentPath, &error));
  auto tailTransition = ComponentIdentityTransition::prepareSaveAs(
      Access::product(tailWindow),
      ProductDocumentIdentity{kTailProject,
                              {"tail", "2026-10-31T00:00:00Z"}});
  CHECK(tailTransition);
  auto tailProjection = Access::prepareIdentityProjection(
      tailWindow, std::move(*tailTransition.transition));
  CHECK(tailProjection && tailProjection->identityTransition());
  const QString tailPath =
      directory.filePath(QStringLiteral("tail-copy.solidar"));
  CHECK(service
            .saveComponentAsIdentityTransition(
                tailPath, Access::product(tailWindow), componentPath,
                *tailProjection->identityTransition())
            .succeeded());
  ComponentIdentityTransitionTestAccess::resetValidationCount();
  Access::installIdentityProjection(tailWindow, std::move(tailProjection));
  CHECK(ComponentIdentityTransitionTestAccess::validationCount() == 0);
  const auto tailSnapshot = Access::viewport(tailWindow)
                                ->componentSession()
                                ->publicationHandle()
                                .snapshotIfCurrent();
  CHECK(tailSnapshot);
  CHECK(tailSnapshot->treeProjection().productIdentity ==
        Access::product(tailWindow).identity());

  // Cancel is an exact no-op. A failed atomic destination also preserves the
  // path, title, identity, dirty bit and incumbent publication.
  const QString pathBefore = window.windowFilePath();
  const QString titleBefore = window.windowTitle();
  const auto identityBefore = *Access::identity(window);
  const auto incarnationBefore = Access::incarnation(window);
  const auto publicationBefore =
      Access::viewport(window)->componentSession()->publicationHandle();
  const bool dirtyBefore = window.isWindowModified();
  bool attempted = true;
  const auto cancelled = Access::saveAs(
      window, [](const QString&) { return std::optional<QString>{}; },
      &attempted);
  CHECK(cancelled.succeeded());
  CHECK(!attempted);
  CHECK(window.windowFilePath() == pathBefore);
  CHECK(*Access::identity(window) == identityBefore);

  const QString impossible = directory.filePath(
      QStringLiteral("missing/subdirectory/failure.solidar"));
  const auto failed = Access::saveAs(
      window,
      [&impossible](const QString&) {
        return std::optional<QString>{impossible};
      },
      &attempted);
  CHECK(!failed.succeeded());
  CHECK(attempted);
  CHECK(window.windowFilePath() == pathBefore);
  CHECK(window.windowTitle() == titleBefore);
  CHECK(*Access::identity(window) == identityBefore);
  CHECK(Access::incarnation(window) == incarnationBefore);
  CHECK(window.isWindowModified() == dirtyBefore);
  CHECK(publicationBefore.isSameCurrentPublication(
      Access::viewport(window)->componentSession()->publicationHandle()));
  CHECK(!Access::viewport(window)->componentSession()->selectedComponent());
  CHECK(!Access::viewport(window)->componentSession()->hoveredComponent());

  auto* preForkSession = Access::viewport(window)->componentSession();
  CHECK(preForkSession && preForkSession->components().size() == 2);
  const auto preForkSelectedAddress = preForkSession->components()[0].address;
  const auto preForkHoveredAddress = preForkSession->components()[1].address;
  const auto preForkSelected = ComponentInstanceSelection::create(
      preForkSession->publicationHandle(), preForkSelectedAddress);
  const auto preForkHovered = ComponentInstanceSelection::create(
      preForkSession->publicationHandle(), preForkHoveredAddress);
  CHECK(preForkSelected && preForkHovered);
  CHECK(Access::viewport(window)->setSelectedComponent(preForkSelected, false));
  CHECK(Access::viewport(window)->setHoveredComponent(preForkHovered, false));

  const QString preparationFailurePath =
      directory.filePath(QStringLiteral("preparation-failure.solidar"));
  Access::failComponentPreparation(window, true);
  const auto preparationFailed = Access::saveAs(
      window,
      [&preparationFailurePath](const QString&) {
        return std::optional<QString>{preparationFailurePath};
      },
      &attempted);
  Access::failComponentPreparation(window, false);
  CHECK(!preparationFailed.succeeded());
  CHECK(attempted);
  CHECK(!QFileInfo::exists(preparationFailurePath));
  CHECK(publicationBefore.isSameCurrentPublication(
      Access::viewport(window)->componentSession()->publicationHandle()));
  CHECK(*Access::identity(window) == identityBefore);
  CHECK(Access::viewport(window)->componentSession()->selectedComponent());
  CHECK(Access::viewport(window)->componentSession()->hoveredComponent());
  CHECK(semanticKey(Access::viewport(window)
                        ->componentSession()
                        ->selectedComponent()
                        ->component()) ==
        semanticKey(preForkSelectedAddress));
  CHECK(semanticKey(Access::viewport(window)
                        ->componentSession()
                        ->hoveredComponent()
                        ->component()) ==
        semanticKey(preForkHoveredAddress));

  const QString forkPath =
      directory.filePath(QStringLiteral("component-copy.solidar"));
  const auto savedAs = Access::saveAs(
      window,
      [&forkPath](const QString&) {
        return std::optional<QString>{forkPath};
      },
      &attempted);
  CHECK(savedAs.succeeded());
  CHECK(attempted);
  CHECK(window.windowFilePath() == forkPath);
  CHECK(window.windowTitle().contains(QStringLiteral("component-copy")));
  CHECK(Access::identity(window)->projectId != identityBefore.projectId);
  CHECK(!publicationBefore.isCurrent());
  const auto* forkSession = Access::viewport(window)->componentSession();
  CHECK(forkSession && forkSession->selectedComponent());
  CHECK(forkSession->hoveredComponent());
  CHECK(semanticKey(forkSession->selectedComponent()->component()) ==
        semanticKey(preForkSelectedAddress));
  CHECK(semanticKey(forkSession->hoveredComponent()->component()) ==
        semanticKey(preForkHoveredAddress));
  const auto forkSnapshot =
      forkSession->publicationHandle().snapshotIfCurrent();
  CHECK(forkSnapshot);
  CHECK(forkSnapshot->treeProjection().productIdentity ==
        std::optional<ProductDocumentIdentity>{*Access::identity(window)});
  const QModelIndex forkRootIndex =
      Access::componentTreeModel(window)->index(0, 0);
  const auto forkRootTreeAddress =
      Access::componentTreeModel(window)->addressForIndex(forkRootIndex);
  CHECK(forkRootTreeAddress);
  CHECK(forkRootTreeAddress->get().publication().isSameCurrentPublication(
      forkSession->publicationHandle()));
  CHECK(Access::componentTree(window)->currentIndex().isValid());
  const auto forked = service.stageOpenProduct(forkPath);
  CHECK(forked.succeeded());
  CHECK(forked.staged->productDocument().projectId() ==
        std::optional<ProjectId>{Access::identity(window)->projectId});

  // Malformed scan and off-side component preparation failures leave the
  // active component carrier and exact publication untouched.
  const QString malformedPath =
      directory.filePath(QStringLiteral("malformed.solidar"));
  QJsonObject malformed = QJsonDocument::fromJson(fixed).object();
  malformed[QStringLiteral("unexpected")] = true;
  writeBytes(malformedPath,
             QJsonDocument(malformed).toJson(QJsonDocument::Compact));
  auto* incumbentSession = Access::viewport(window)->componentSession();
  CHECK(incumbentSession);
  CHECK(!incumbentSession->components().empty());
  const auto selectedAddress = incumbentSession->components().front().address;
  const auto selected = ComponentInstanceSelection::create(
      incumbentSession->publicationHandle(), selectedAddress);
  CHECK(selected);
  CHECK(Access::viewport(window)->setSelectedComponent(selected, false));
  const auto incumbentIncarnation = Access::incarnation(window);
  const auto incumbentPublication =
      Access::viewport(window)->componentSession()->publicationHandle();
  CHECK(!window.loadProject(malformedPath, &error));
  CHECK(Access::incarnation(window) == incumbentIncarnation);
  CHECK(incumbentPublication.isSameCurrentPublication(
      Access::viewport(window)->componentSession()->publicationHandle()));
  CHECK(Access::viewport(window)->selectedComponentView());
  CHECK(Access::viewport(window)->selectedComponentView()->component() ==
        selectedAddress);
  Access::failComponentPreparation(window, true);
  CHECK(!window.loadProject(componentPath, &error));
  Access::failComponentPreparation(window, false);
  CHECK(Access::incarnation(window) == incumbentIncarnation);
  CHECK(incumbentPublication.isSameCurrentPublication(
      Access::viewport(window)->componentSession()->publicationHandle()));
  CHECK(Access::viewport(window)->selectedComponentView());
  CHECK(Access::viewport(window)->selectedComponentView()->component() ==
        selectedAddress);

  // Compatibility switching remains distinct for Part, V10/V11 Component
  // workspaces using ModelRibbon, and legacy V7 Assembly using its ribbon.
  CHECK(window.loadProject(partPath, &error));
  CHECK(Access::rootKind(window) == DefinitionKind::Part);
  CHECK(!Access::viewport(window)->componentSession());
  const auto partIncarnation = Access::incarnation(window);
  Access::failComponentPreparation(window, true);
  CHECK(!window.loadProject(componentPath, &error));
  CHECK(Access::rootKind(window) == DefinitionKind::Part);
  CHECK(Access::incarnation(window) == partIncarnation);
  Access::failComponentPreparation(window, false);
  CHECK(window.loadProject(componentPath, &error));
  CHECK(Access::rootKind(window) == DefinitionKind::Component);
  CHECK(Access::currentRibbon(window) == Access::modelRibbon(window));
  CHECK(window.loadProject(componentV11Path, &error));
  CHECK(Access::rootKind(window) == DefinitionKind::Component);
  CHECK(Access::persistence(window).format ==
        project::ProjectPersistenceFormat::V11);
  CHECK(Access::drawingNodeCount(window) == 2);
  CHECK(Access::currentRibbon(window) == Access::modelRibbon(window));
  CHECK(window.loadProject(assemblyPath, &error));
  CHECK(Access::rootKind(window) == DefinitionKind::Assembly);
  CHECK(!Access::viewport(window)->componentSession());
  CHECK(Access::currentRibbon(window) == Access::assemblyRibbon(window));
  const auto assemblyIncarnation = Access::incarnation(window);
  const auto assemblyPublication =
      Access::viewport(window)->assemblySession()->publicationHandle();
  Access::failComponentPreparation(window, true);
  CHECK(!window.loadProject(componentPath, &error));
  CHECK(Access::rootKind(window) == DefinitionKind::Assembly);
  CHECK(Access::incarnation(window) == assemblyIncarnation);
  CHECK(assemblyPublication.isSameCurrentPublication(
      Access::viewport(window)->assemblySession()->publicationHandle()));
  Access::failComponentPreparation(window, false);
  CHECK(window.loadProject(componentPath, &error));
  CHECK(Access::rootKind(window) == DefinitionKind::Component);
  CHECK(Access::viewport(window)->componentSession());

  // The first extraction activates the new root component. Further root-body
  // extractions keep that same root active and remain available from the
  // visible component tree. Undo/Redo crosses the Part/Component boundary
  // without ever publishing an empty white view.
  {
    MainWindow extractionWindow(settings);
    CHECK(!extractionWindow.findChild<QAction*>(
        QStringLiteral("createAssemblyProjectAction")));
    CHECK(Access::installPartProduct(extractionWindow, twoBodyPartProduct(),
                                     BodyId{201}));
    CHECK(Access::extractionEnabled(extractionWindow));
    Access::extract(extractionWindow);
    CHECK(Access::rootKind(extractionWindow) == DefinitionKind::Component);
    CHECK(Access::componentEditActive(extractionWindow));
    CHECK(Access::activeComponentDefinition(extractionWindow) ==
          std::optional<DefinitionId>{DefinitionId{1}});
    CHECK(Access::activeComponentHasBody(extractionWindow, BodyId{202}));
    CHECK(Access::componentEditContextVisible(extractionWindow));
    CHECK(!Access::componentEditContextGhosted(extractionWindow));
    CHECK(Access::authoringEnabled(extractionWindow));
    CHECK(Access::selectComponentBody(extractionWindow, BodyId{202}, true));
    CHECK(Access::extractionEnabled(extractionWindow));
    CHECK(Access::extractionOverflowEnabled(extractionWindow));
    Access::leaveComponent(extractionWindow);
    CHECK(Access::persistence(extractionWindow).format ==
          project::ProjectPersistenceFormat::V10);
    CHECK(Access::definitionCount(extractionWindow) == 2);
    CHECK(Access::rootComponent(extractionWindow));
    CHECK(Access::rootComponent(extractionWindow)->find(OccurrenceId{1}));
    CHECK(Access::rootComponent(extractionWindow)
              ->find(OccurrenceId{1})
              ->targetDefinitionId() == DefinitionId{2});
    CHECK(Access::viewport(extractionWindow)->componentSession());
    CHECK(Access::viewport(extractionWindow)
              ->componentSession()
              ->scene()
              ->instances()
              .size() == 2);
    CHECK(!Access::extractionEnabled(extractionWindow));
    CHECK(Access::selectComponentBody(extractionWindow, BodyId{202}, true));
    CHECK(Access::extractionEnabled(extractionWindow));
    CHECK(Access::extractionOverflowEnabled(extractionWindow));
    CHECK(Access::selectDescendantFromViewport(extractionWindow));
    CHECK(!Access::extractionEnabled(extractionWindow));
    CHECK(!Access::extractionOverflowEnabled(extractionWindow));
    CHECK(Access::hoverSelectedComponent(extractionWindow));
    const auto hoverBefore = Access::hoveredComponent(extractionWindow);
    CHECK(hoverBefore);
    const auto placementBefore =
        Access::occurrencePlacement(extractionWindow, OccurrenceId{1});
    CHECK(placementBefore);
    CHECK(*placementBefore == RigidTransform3d::identity());
    const auto failedPlacementRequest = Access::placementRequest(
        extractionWindow, std::array<double, 3>{25.0, 0.0, 0.0});
    CHECK(failedPlacementRequest);
    const auto incumbentPublication =
        Access::viewport(extractionWindow)
            ->componentSession()
            ->publicationHandle();
    Access::failExtractionProjection(extractionWindow, true);
    Access::requestPlacement(extractionWindow, *failedPlacementRequest);
    Access::failExtractionProjection(extractionWindow, false);
    CHECK(Access::modelUndoCount(extractionWindow) == 1);
    CHECK(Access::occurrencePlacement(extractionWindow, OccurrenceId{1}) ==
          placementBefore);
    CHECK(incumbentPublication.isSameCurrentPublication(
        Access::viewport(extractionWindow)
            ->componentSession()
            ->publicationHandle()));
    CHECK(Access::hoveredComponent(extractionWindow) == hoverBefore);

    const auto placementRequest = Access::placementRequest(
        extractionWindow, std::array<double, 3>{25.0, 0.0, 0.0});
    CHECK(placementRequest);
    Access::requestPlacement(extractionWindow, *placementRequest);
    const auto placementAfter =
        Access::occurrencePlacement(extractionWindow, OccurrenceId{1});
    CHECK(placementAfter);
    CHECK((placementAfter->translationMm() ==
           std::array<double, 3>{25.0, 0.0, 0.0}));
    CHECK(Access::modelUndoCount(extractionWindow) == 2);
    CHECK(Access::viewport(extractionWindow)->selectedComponentView());
    CHECK(Access::viewport(extractionWindow)
              ->selectedComponentView()
              ->component()
              .occurrencePath ==
          placementRequest->component.occurrencePath);
    const auto hoverAfter = Access::hoveredComponent(extractionWindow);
    CHECK(hoverAfter);
    CHECK(hoverAfter->definitionId == placementRequest->component.definitionId);
    CHECK(hoverAfter->occurrencePath ==
          placementRequest->component.occurrencePath);

    // The old request is sealed to the retired publication and is an exact
    // no-op after the successful projection swap.
    Access::requestPlacement(extractionWindow, *placementRequest);
    CHECK(Access::modelUndoCount(extractionWindow) == 2);
    CHECK(Access::occurrencePlacement(extractionWindow, OccurrenceId{1}) ==
          placementAfter);
    CHECK(Access::clearComponentFromViewport(extractionWindow));
    CHECK(!Access::extractionEnabled(extractionWindow));
    CHECK(!Access::extractionOverflowEnabled(extractionWindow));
    CHECK(Access::selectComponentBody(extractionWindow, BodyId{202}, true));
    CHECK(Access::extractionEnabled(extractionWindow));
    CHECK(Access::extractionOverflowEnabled(extractionWindow));
    Access::extract(extractionWindow);
    CHECK(Access::componentEditActive(extractionWindow));
    CHECK(Access::activeComponentDefinition(extractionWindow) ==
          std::optional<DefinitionId>{DefinitionId{1}});
    Access::leaveComponent(extractionWindow);
    CHECK(Access::definitionCount(extractionWindow) == 3);
    CHECK(Access::rootComponent(extractionWindow)->find(OccurrenceId{2}));
    CHECK(Access::rootComponent(extractionWindow)
              ->find(OccurrenceId{2})
              ->targetDefinitionId() == DefinitionId{3});
    CHECK(Access::viewport(extractionWindow)
              ->componentSession()
              ->scene()
              ->instances()
              .size() == 2);
    CHECK(Access::modelUndoCount(extractionWindow) == 3);

    Access::undo(extractionWindow);
    CHECK(Access::rootKind(extractionWindow) == DefinitionKind::Component);
    CHECK(Access::definitionCount(extractionWindow) == 2);
    CHECK(Access::viewport(extractionWindow)->componentSession());
    Access::undo(extractionWindow);
    CHECK(Access::rootKind(extractionWindow) == DefinitionKind::Component);
    CHECK(Access::definitionCount(extractionWindow) == 2);
    CHECK(Access::occurrencePlacement(extractionWindow, OccurrenceId{1}) ==
          placementBefore);
    CHECK(Access::viewport(extractionWindow)->selectedComponentView());
    Access::undo(extractionWindow);
    CHECK(Access::rootKind(extractionWindow) == DefinitionKind::Part);
    CHECK(Access::persistence(extractionWindow).format ==
          project::ProjectPersistenceFormat::V5);
    CHECK(!Access::viewport(extractionWindow)->componentSession());
    CHECK(Access::viewport(extractionWindow)->displayedBodyCount() == 2);

    Access::redo(extractionWindow);
    CHECK(Access::rootKind(extractionWindow) == DefinitionKind::Component);
    CHECK(Access::definitionCount(extractionWindow) == 2);
    CHECK(Access::viewport(extractionWindow)->componentSession());
    Access::redo(extractionWindow);
    CHECK(Access::rootKind(extractionWindow) == DefinitionKind::Component);
    CHECK(Access::definitionCount(extractionWindow) == 2);
    CHECK(Access::occurrencePlacement(extractionWindow, OccurrenceId{1}) ==
          placementAfter);
    CHECK(Access::viewport(extractionWindow)->selectedComponentView());
    Access::redo(extractionWindow);
    CHECK(Access::rootKind(extractionWindow) == DefinitionKind::Component);
    CHECK(Access::definitionCount(extractionWindow) == 3);
    CHECK(Access::rootComponent(extractionWindow)->find(OccurrenceId{1}));
    CHECK(Access::rootComponent(extractionWindow)->find(OccurrenceId{2}));
    CHECK(Access::viewport(extractionWindow)->componentSession());

    const QString extractedPath =
        directory.filePath(QStringLiteral("extracted-components.solidar"));
    QString extractionSaveError;
    CHECK(Access::saveTo(extractionWindow, extractedPath,
                         &extractionSaveError));
    CHECK(extractionSaveError.isEmpty());
    MainWindow reopenedExtraction(settings);
    CHECK(reopenedExtraction.loadProject(extractedPath,
                                         &extractionSaveError));
    CHECK(Access::rootKind(reopenedExtraction) == DefinitionKind::Component);
    CHECK(Access::definitionCount(reopenedExtraction) == 3);
    CHECK(Access::rootComponent(reopenedExtraction)->find(OccurrenceId{1}));
    CHECK(Access::rootComponent(reopenedExtraction)->find(OccurrenceId{2}));
    CHECK(Access::occurrencePlacement(reopenedExtraction, OccurrenceId{1}) ==
          placementAfter);
    CHECK(Access::viewport(reopenedExtraction)->componentSession());
  }

  // The same command remains available from the visible tree inside an active
  // child component, produces a second-level occurrence, and leaves the owner
  // component active.
  {
    MainWindow nestedExtraction(settings);
    CHECK(Access::installPartProduct(nestedExtraction, twoBodyPartProduct(),
                                     BodyId{201}));
    Access::extract(nestedExtraction);
    CHECK(Access::componentEditActive(nestedExtraction));
    CHECK(Access::activeComponentDefinition(nestedExtraction) ==
          std::optional<DefinitionId>{DefinitionId{1}});
    auto firstLevelPath = OccurrencePath::create({OccurrenceId{1}});
    CHECK(firstLevelPath);
    const QModelIndex firstLevelComponent =
        Access::componentIndex(nestedExtraction, DefinitionId{2},
                               firstLevelPath.path);
    CHECK(firstLevelComponent.isValid());
    CHECK(Access::enterComponent(nestedExtraction, firstLevelComponent));
    CHECK(Access::activeComponentDefinition(nestedExtraction) ==
          std::optional<DefinitionId>{DefinitionId{2}});
    CHECK(Access::selectComponentBody(nestedExtraction, BodyId{201}, false));
    CHECK(Access::createComponentVisible(nestedExtraction));
    CHECK(Access::extractionEnabled(nestedExtraction));
    Access::extract(nestedExtraction);
    CHECK(Access::componentEditActive(nestedExtraction));
    CHECK(Access::activeComponentDefinition(nestedExtraction) ==
          std::optional<DefinitionId>{DefinitionId{2}});
    CHECK(!Access::activeComponentHasBody(nestedExtraction, BodyId{201}));
    Access::leaveComponent(nestedExtraction);
    CHECK(Access::definitionCount(nestedExtraction) == 3);
    const auto* nestedOwner =
        Access::product(nestedExtraction)
            .definitions()
            .findComponent(DefinitionId{2});
    CHECK(nestedOwner && nestedOwner->find(OccurrenceId{2}));
    CHECK(nestedOwner->find(OccurrenceId{2})->targetDefinitionId() ==
          DefinitionId{3});
    CHECK(Access::viewport(nestedExtraction)->componentSession());
    auto nestedPath = OccurrencePath::create(
        {OccurrenceId{1}, OccurrenceId{2}});
    CHECK(nestedPath);
    CHECK(Access::componentIndex(nestedExtraction, DefinitionId{3},
                                 nestedPath.path)
              .isValid());
    CHECK(Access::modelUndoCount(nestedExtraction) == 2);
    Access::undo(nestedExtraction);
    CHECK(Access::definitionCount(nestedExtraction) == 2);
    nestedOwner = Access::product(nestedExtraction)
                      .definitions()
                      .findComponent(DefinitionId{2});
    CHECK(nestedOwner && nestedOwner->document().findBody(BodyId{201}));
    CHECK(!nestedOwner->find(OccurrenceId{2}));
    Access::redo(nestedExtraction);
    CHECK(Access::definitionCount(nestedExtraction) == 3);
    nestedOwner = Access::product(nestedExtraction)
                      .definitions()
                      .findComponent(DefinitionId{2});
    CHECK(nestedOwner && nestedOwner->find(OccurrenceId{2}));
  }

  // Off-side projection failure is an exact no-op for model, history,
  // persistence and the incumbent Part body display.
  {
    MainWindow rejected(settings);
    CHECK(Access::installPartProduct(rejected, twoBodyPartProduct(),
                                     BodyId{201}));
    const auto incarnation = Access::incarnation(rejected);
    const auto undoCount = Access::modelUndoCount(rejected);
    const auto persistence = Access::persistence(rejected);
    CHECK(Access::viewport(rejected)->displayedBodyCount() == 2);
    Access::failExtractionProjection(rejected, true);
    Access::extract(rejected);
    Access::failExtractionProjection(rejected, false);
    CHECK(Access::rootKind(rejected) == DefinitionKind::Part);
    CHECK(Access::incarnation(rejected) == incarnation);
    CHECK(Access::definitionCount(rejected) == 1);
    CHECK(Access::modelUndoCount(rejected) == undoCount);
    CHECK(Access::persistence(rejected) == persistence);
    CHECK(Access::viewport(rejected)->displayedBodyCount() == 2);
  }

  // A sketch attached to the selected body's face no longer disables the
  // command. Extraction keeps the retained geometry in place by converting
  // only that external support to a fixed placement; Undo/Redo restores the
  // exact authored/fixed states.
  {
    MainWindow supported(settings);
    CHECK(Access::installPartProduct(supported, faceSupportedPartProduct(),
                                     BodyId{10}));
    const auto* beforeRoot = Access::product(supported).rootPartDefinition();
    CHECK(beforeRoot);
    const auto* beforeSketch = beforeRoot->document().findSketch(SketchId{30});
    CHECK(beforeSketch);
    const auto placementBefore = beforeSketch->placement;
    CHECK(beforeSketch->support.type == SketchSupportType::Face);
    CHECK(Access::extractionEnabled(supported));
    CHECK(Access::extractionToolTip(supported).contains(
        QString::fromUtf8("зафиксирована"), Qt::CaseInsensitive));

    Access::extract(supported);
    CHECK(Access::rootKind(supported) == DefinitionKind::Component);
    CHECK(Access::componentEditActive(supported));
    Access::leaveComponent(supported);
    CHECK(Access::modelUndoCount(supported) == 1);
    const auto rootId = Access::product(supported).rootDefinitionId();
    const auto* rootDocument =
        Access::product(supported).componentDocument(rootId);
    const auto* childDocument = Access::product(supported).componentDocument(
        DefinitionId{2});
    CHECK(rootDocument);
    CHECK(childDocument);
    CHECK(rootDocument->findBody(BodyId{11}));
    CHECK(childDocument->findBody(BodyId{10}));
    const auto* fixedSketch = rootDocument->findSketch(SketchId{30});
    CHECK(fixedSketch);
    CHECK(fixedSketch->support.type == SketchSupportType::BasePlane);
    CHECK(samePlacement(fixedSketch->placement, placementBefore));

    Access::undo(supported);
    CHECK(Access::rootKind(supported) == DefinitionKind::Part);
    const auto* undoneSketch = Access::product(supported)
                                   .rootPartDefinition()
                                   ->document()
                                   .findSketch(SketchId{30});
    CHECK(undoneSketch);
    CHECK(undoneSketch->support.type == SketchSupportType::Face);
    CHECK(samePlacement(undoneSketch->placement, placementBefore));

    Access::redo(supported);
    CHECK(Access::rootKind(supported) == DefinitionKind::Component);
    const auto* redoneSketch = Access::product(supported)
                                   .componentDocument(
                                       Access::product(supported)
                                           .rootDefinitionId())
                                   ->findSketch(SketchId{30});
    CHECK(redoneSketch);
    CHECK(redoneSketch->support.type == SketchSupportType::BasePlane);
    CHECK(samePlacement(redoneSketch->placement, placementBefore));
  }

  // Authoritative dependency planning disables the command and exposes a
  // useful diagnostic; even a direct internal invocation remains an exact
  // no-op.
  {
    MainWindow dependent(settings);
    CHECK(Access::installPartProduct(dependent, sharedSketchPartProduct(),
                                     BodyId{10}));
    CHECK(!Access::extractionEnabled(dependent));
    CHECK(Access::extractionToolTip(dependent).contains(
        QString::fromUtf8("связано"), Qt::CaseInsensitive));
    const auto undoCount = Access::modelUndoCount(dependent);
    Access::extract(dependent);
    CHECK(Access::rootKind(dependent) == DefinitionKind::Part);
    CHECK(Access::definitionCount(dependent) == 1);
    CHECK(Access::modelUndoCount(dependent) == undoCount);
    CHECK(Access::viewport(dependent)->displayedBodyCount() == 2);
  }

  return EXIT_SUCCESS;
}
