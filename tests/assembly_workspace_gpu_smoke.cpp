#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QTreeView>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <array>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AssemblyGpuSmokeClassification.h"
#include "TestAssertions.h"
#include "app/AppSettings.h"
#include "model/IdGeneration.h"
#include "model/ImportedShapeFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/MoveFeature.h"
#include "model/OccurrenceDelta.h"
#include "project/ProjectFile.h"
#include "project/LinkedPartSourceResolver.h"
#include "project/ProductEnvelopeV5Encoder.h"
#include "project/ProductEnvelopeV6Encoder.h"
#include "project/ProductEnvelopeV7Encoder.h"
#include "project/ProductEnvelopeV8Encoder.h"
#include "ui/AssemblyTreeModel.h"
#include "ui/AssemblyViewportFit.h"
#include "ui/AssemblyViewportRuler.h"
#include "ui/MainWindow.h"
#include "ui/ModelRibbon.h"
#include "ui/Viewport.h"
#include "ui/application/AssemblyInsertToolSession.h"
#include "ui/application/DocumentDelta.h"
#include "ui/application/ProjectApplicationService.h"

namespace solidar {

class MainWindowUndoTestAccess final {
 public:
  static const ProductDocument& product(const MainWindow& window) noexcept {
    return window.productDocument_;
  }
  static ProductDocument& mutableProduct(MainWindow& window) noexcept {
    return window.productDocument_;
  }
  static const AssemblyCommandHistory& history(
      const MainWindow& window) noexcept {
    return window.assemblyCommandHistory_;
  }
  static void injectProjection(
      MainWindow& window,
      AssemblyViewportSessionFailurePoint point) noexcept {
    window.assemblyPreparationFailurePoint_ = point;
  }
  static const Document* partDocument(
      const MainWindow& window, DefinitionId id) noexcept {
    return window.productDocument_.partDocument(id);
  }
  static DefinitionStateId definitionState(
      const MainWindow& window, DefinitionId id) noexcept {
    return window.productDocument_.definitionStateId(id);
  }
  static bool definitionEditActive(const MainWindow& window) noexcept {
    return window.definitionEditContext_.has_value();
  }
  static const AssemblyViewportSession* suspendedAssemblySession(
      const MainWindow& window) noexcept {
    return window.suspendedAssemblySession_.get();
  }
  static std::size_t modelHistorySize(const MainWindow& window) noexcept {
    return window.modelHistory_.undoCount() + window.modelHistory_.redoCount();
  }
  static void injectDefinitionMoveRecomputeFailure(MainWindow& window,
                                                   bool enabled) noexcept {
    window.definitionMoveRecomputeFailureForTesting_ = enabled;
  }
  static bool commitImportedBody(MainWindow& window,
                                 const std::string& name,
                                 double extent) {
    if (!window.definitionEditContext_) return false;
    const Document* current = window.productDocument_.partDocument(
        window.definitionEditContext_->targetDefinitionId);
    if (!current) return false;
    Document candidate = *current;
    auto& body = candidate.addBody(name);
    body.addFeature(std::make_unique<ImportedShapeFeature>(
        std::make_shared<const TopoDS_Shape>(
            BRepPrimAPI_MakeBox(extent, extent * 0.5, extent * 0.25).Shape()),
        name + " feature"));
    return window.commitDefinitionCandidate(std::move(candidate));
  }
  static bool rejectedInvalidEditPreservesDocument(MainWindow& window) {
    const Document before = window.activePartDocument();
    const Document* const beforeAddress = &window.activePartDocument();
    const auto stateBefore = window.productDocument_.definitionStateId(
        window.definitionEditContext_->targetDefinitionId);
    const auto historySizeBefore = window.assemblyCommandHistory_.size();
    const auto historyCursorBefore = window.assemblyCommandHistory_.cursor();
    const auto historyRevisionBefore =
        window.assemblyCommandHistory_.currentRevision();
    const auto savepointBefore =
        window.assemblyCommandHistory_.savepointRevision();
    const bool modifiedBefore = window.isWindowModified();
    Document candidate = before;
    auto& body = candidate.addBody("A5d invalid body");
    body.addFeature(std::make_unique<ImportedShapeFeature>(
        std::shared_ptr<const TopoDS_Shape>{}, "A5d invalid feature"));
    const bool rejected = !window.commitDefinitionCandidate(
        std::move(candidate));
    const bool exactDocument =
        DocumentDelta::create(before, window.activePartDocument()).empty();
    return rejected && &window.activePartDocument() == beforeAddress && exactDocument &&
           window.productDocument_.definitionStateId(
               window.definitionEditContext_->targetDefinitionId) ==
               stateBefore &&
           window.assemblyCommandHistory_.size() == historySizeBefore &&
           window.assemblyCommandHistory_.cursor() == historyCursorBefore &&
           window.assemblyCommandHistory_.currentRevision() ==
               historyRevisionBefore &&
           window.assemblyCommandHistory_.savepointRevision() ==
               savepointBefore &&
           window.isWindowModified() == modifiedBefore;
  }
  static void undo(MainWindow& window) { window.undoLastAction(); }
  static void redo(MainWindow& window) { window.redoLastAction(); }
  static PartDesignToolKind activeTool(const MainWindow& window) noexcept {
    return window.partDesignCoordinator_.activeTool();
  }
  static bool previewHasPending(const MainWindow& window) noexcept {
    return window.previewUpdates_ && window.previewUpdates_->hasPending();
  }
  static PreviewUpdateCoordinator::Counters previewCounters(
      const MainWindow& window) noexcept {
    return window.previewUpdates_ ? window.previewUpdates_->counters()
                                  : PreviewUpdateCoordinator::Counters{};
  }
  static void enterDefinitionEdit(MainWindow& window) {
    window.enterAssemblyDefinitionEdit();
  }
  static void leaveDefinitionEdit(MainWindow& window) {
    window.leaveAssemblyDefinitionEdit();
  }
  static ViewportCameraState assemblyCamera(
      const MainWindow& window) noexcept {
    return window.viewport_->assemblyCamera();
  }
  static void reloadSelectedLinkedPart(MainWindow& window) {
    window.reloadSelectedLinkedPart();
  }
  static LinkedPartRuntimeStatus linkedRuntimeStatus(
      const MainWindow& window, DefinitionId id) noexcept {
    const auto found = window.linkedPartRuntimeStates_.find(id);
    return found == window.linkedPartRuntimeStates_.end()
               ? LinkedPartRuntimeStatus::Unchecked
               : found->second.status;
  }
  static project::ProjectPersistenceFormat persistenceFormat(
      const MainWindow& window) noexcept {
    return window.projectPersistenceState_.format;
  }
  static void beginMate(MainWindow& window) { window.beginAssemblyMate(); }
  static void cancelMate(MainWindow& window) { window.cancelAssemblyMate(); }
  static void applyMate(MainWindow& window) { window.applyAssemblyMate(); }
  static void escapeMate(MainWindow& window) {
    window.assemblyMateController_->session().escape();
    window.updateAssemblyMateUi();
  }
  static AssemblyMateToolState mateState(const MainWindow& window) {
    return window.assemblyMateController_->session().state();
  }
  static bool canApplyMate(const MainWindow& window) {
    return window.assemblyMateController_->session().canApply();
  }
  static std::vector<AssemblyFaceRayHit> mateHits(MainWindow& window) {
    std::vector<AssemblyFaceRayHit> result;
    const auto* session = window.viewport_->assemblySession();
    if (!session) return result;
    for (const auto& occurrence : session->occurrences()) {
      const auto resolved =
          window.productDocument_.resolveOccurrence(occurrence.identity.path);
      if (!resolved || !resolved.targetPart) continue;
      const auto& document = resolved.targetPart->document();
      bool added = false;
      for (const auto& body : document.bodies()) {
        for (const auto& feature : body.features()) {
          for (std::size_t index = 0; index < 64; ++index) {
            const auto face = document.createFaceReferenceV2(
                body.id(), feature->id(), index);
            if (!face) continue;
            MateEndpoint endpoint{occurrence.identity.path, face.reference};
            if (!classifyPlanarMateEndpoint(
                    window.productDocument_,
                    window.productDocument_.rootDefinitionId(), endpoint))
              continue;
            result.push_back({{occurrence.identity,
                               resolved.targetPart->id(), face.reference},
                              {}, 0.0, 0.0,
                              session->publicationHandle()});
            added = true;
            break;
          }
          if (added) break;
        }
        if (added) break;
      }
    }
    return result;
  }
  static void acceptMate(MainWindow& window, const AssemblyFaceRayHit& hit) {
    window.acceptAssemblyMatePick(hit);
  }
  static void setMateOpposed(MainWindow& window, bool value) {
    window.assemblyMateOpposedCheck_->setChecked(value);
  }
  static void failMatePreview(MainWindow& window) {
    window.assemblyMatePreviewPrepareFailureForTesting_ = true;
  }
  static void setMateReentryHook(MainWindow& window,
                                 std::function<void()> hook) {
    window.assemblyMateReentryHookForTesting_ = std::move(hook);
  }
  static bool mateApplyVisibleAndEnabled(const MainWindow& window) {
    return window.assemblyApplyButton_->isVisible() &&
           window.assemblyApplyButton_->isEnabled();
  }
  static const AssemblyViewportScene* matePreviewScene(
      const MainWindow& window) {
    return window.viewport_->assemblyMatePreviewScene().get();
  }
  static bool mateControlsHaveValidGeometry(const MainWindow& window) {
    const auto inside = [&](const QWidget* child) {
      return child && child->isVisible() && child->width() > 0 &&
             child->height() > 0 &&
             window.assemblyRibbon_->rect().contains(
                 child->geometry().topLeft()) &&
             window.assemblyRibbon_->rect().contains(
                 child->geometry().bottomRight());
    };
    return inside(window.assemblyMateOpposedCheck_) &&
           inside(window.assemblyMateStatusLabel_) &&
           inside(window.assemblyApplyButton_) &&
           inside(window.assemblyCancelButton_) &&
           !window.assemblyMateOpposedCheck_->geometry().intersects(
               window.assemblyApplyButton_->geometry());
  }
  static std::size_t mateCount(const MainWindow& window) {
    const auto* root = window.productDocument_.rootAssemblyDefinition();
    return root ? root->mateRegistry().size() : 0;
  }
};

}  // namespace solidar

namespace {

using namespace solidar;

void marker(const char* name, bool passed) {
  std::cout << name << '=' << (passed ? "PASS" : "FAIL") << '\n';
}

bool disabled(QObject& owner, const char* objectName) {
  const auto* action = owner.findChild<QAction*>(QLatin1String(objectName));
  return action && !action->isEnabled();
}

bool writeSharedSourceAssemblyFixture(const QString& path, Document part) {
  constexpr ProjectId kProject{
      {0x87, 0x44, 0x22, 0x51, 0x12, 0x6e, 0x4b, 0xa2,
       0x9b, 0x14, 0x19, 0x61, 0xd8, 0x5b, 0xc0, 0x02}};
  constexpr DefinitionId kPart{1};
  constexpr DefinitionId kOrphan{2};
  constexpr DefinitionId kRoot{100};

  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document orphan = part;
  DefinitionRegistry definitions;
  if (definitions.insert(PartDefinition(
          kPart, std::make_unique<Document>(std::move(part)))) !=
      DefinitionInsertStatus::Inserted)
    return false;
  if (definitions.insert(PartDefinition(
          kOrphan, std::make_unique<Document>(std::move(orphan)))) !=
      DefinitionInsertStatus::Inserted)
    return false;
  const auto secondPlacement = RigidTransform3d::create(
      {40.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  if (!secondPlacement) return false;
  AssemblyDefinition root(kRoot);
  if (root.insert(ComponentOccurrence(OccurrenceId{11}, kPart,
                                      RigidTransform3d::identity())) !=
          AssemblyOccurrenceInsertStatus::Inserted ||
      root.insert(ComponentOccurrence(OccurrenceId{12}, kPart,
                                      *secondPlacement.transform)) !=
          AssemblyOccurrenceInsertStatus::Inserted ||
      root.insert(ComponentOccurrence(
          OccurrenceId{13}, kPart, RigidTransform3d::identity(),
          {.visible = false, .suppressed = true, .grounded = false})) !=
          AssemblyOccurrenceInsertStatus::Inserted ||
      definitions.insert(std::move(root)) != DefinitionInsertStatus::Inserted)
    return false;
  auto product = ProductDocument::create(std::move(definitions), kRoot);
  if (!product) return false;
  const auto encoded = project::encodeProductEnvelopeV6Root(
      *product.product, kProject,
      {QStringLiteral("shared-source-assembly"),
       QStringLiteral("2026-10-25T00:00:00Z")});
  if (!encoded.succeeded()) return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(encoded.payload->bytes) == encoded.payload->bytes.size();
}

bool writeDefinitionEditAssemblyFixture(const QString& path, Document part) {
  constexpr ProjectId kProject{
      {0x93, 0x24, 0x62, 0x11, 0x82, 0x7e, 0x4b, 0xb2,
       0x8b, 0x34, 0x29, 0x71, 0xc8, 0x6b, 0xd0, 0x12}};
  constexpr DefinitionId kEdited{1};
  constexpr DefinitionId kOrphan{2};
  constexpr DefinitionId kUnaffected{3};
  constexpr DefinitionId kRoot{100};

  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document orphan = part;
  Document unaffected = part;
  DefinitionRegistry definitions;
  if (definitions.insert(PartDefinition(
          kEdited, std::make_unique<Document>(std::move(part)))) !=
          DefinitionInsertStatus::Inserted ||
      definitions.insert(PartDefinition(
          kOrphan, std::make_unique<Document>(std::move(orphan)))) !=
          DefinitionInsertStatus::Inserted ||
      definitions.insert(PartDefinition(
          kUnaffected, std::make_unique<Document>(std::move(unaffected)))) !=
          DefinitionInsertStatus::Inserted)
    return false;
  const auto secondPlacement = RigidTransform3d::create(
      {40.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  const auto unaffectedPlacement = RigidTransform3d::create(
      {80.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  if (!secondPlacement || !unaffectedPlacement) return false;
  AssemblyDefinition root(kRoot);
  if (root.insert(ComponentOccurrence(
          OccurrenceId{11}, kEdited, RigidTransform3d::identity(),
          {.visible = true, .suppressed = false, .grounded = false})) != AssemblyOccurrenceInsertStatus::Inserted ||
      root.insert(ComponentOccurrence(
          OccurrenceId{12}, kEdited, *secondPlacement.transform,
          {.visible = true, .suppressed = false, .grounded = true})) != AssemblyOccurrenceInsertStatus::Inserted ||
      root.insert(ComponentOccurrence(
          OccurrenceId{13}, kEdited, RigidTransform3d::identity(),
          {.visible = false, .suppressed = true, .grounded = false})) != AssemblyOccurrenceInsertStatus::Inserted ||
      root.insert(ComponentOccurrence(
          OccurrenceId{14}, kUnaffected, *unaffectedPlacement.transform,
          {.visible = true, .suppressed = false, .grounded = false})) != AssemblyOccurrenceInsertStatus::Inserted ||
      definitions.insert(std::move(root)) != DefinitionInsertStatus::Inserted)
    return false;
  auto product = ProductDocument::create(std::move(definitions), kRoot);
  if (!product) return false;
  const auto encoded = project::encodeProductEnvelopeV6Root(
      *product.product, kProject,
      {QStringLiteral("definition-edit-assembly"),
       QStringLiteral("2026-10-26T00:00:00Z")});
  if (!encoded.succeeded()) return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(encoded.payload->bytes) == encoded.payload->bytes.size();
}

bool writeRemoveAssemblyFixture(const QString& path, Document part) {
  constexpr ProjectId kProject{
      {0x43, 0x54, 0x32, 0x21, 0x72, 0x1e, 0x4a, 0xc2,
       0xab, 0x44, 0x39, 0x51, 0xb8, 0x7b, 0xe0, 0x22}};
  constexpr DefinitionId kPart{1};
  constexpr DefinitionId kOrphan{2};
  constexpr DefinitionId kRoot{100};

  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document orphan = part;
  DefinitionRegistry definitions;
  if (definitions.insert(PartDefinition(
          kPart, std::make_unique<Document>(std::move(part)))) !=
          DefinitionInsertStatus::Inserted ||
      definitions.insert(PartDefinition(
          kOrphan, std::make_unique<Document>(std::move(orphan)))) !=
          DefinitionInsertStatus::Inserted)
    return false;
  const auto secondPlacement = RigidTransform3d::create(
      {40.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  if (!secondPlacement) return false;
  AssemblyDefinition root(kRoot);
  if (root.insert(ComponentOccurrence(
          OccurrenceId{11}, kPart, RigidTransform3d::identity())) !=
          AssemblyOccurrenceInsertStatus::Inserted ||
      root.insert(ComponentOccurrence(
          OccurrenceId{12}, kPart, *secondPlacement.transform)) !=
          AssemblyOccurrenceInsertStatus::Inserted ||
      definitions.insert(std::move(root)) != DefinitionInsertStatus::Inserted)
    return false;
  auto product = ProductDocument::create(std::move(definitions), kRoot);
  if (!product) return false;
  const auto encoded = project::encodeProductEnvelopeV6Root(
      *product.product, kProject,
      {QStringLiteral("remove-assembly"),
       QStringLiteral("2026-10-26T00:00:00Z")});
  if (!encoded.succeeded()) return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(encoded.payload->bytes) == encoded.payload->bytes.size();
}

constexpr ProjectId kA6b2SourceProject{
    {0x42, 0x18, 0x25, 0x3a, 0x49, 0x5b, 0x4c, 0x8d,
     0x9e, 0xaf, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5, 0x16}};
constexpr ProjectId kA6b2AssemblyProject{
    {0x43, 0x29, 0x36, 0x4b, 0x5a, 0x6c, 0x4d, 0x9e,
     0xaf, 0xb0, 0xc2, 0xd3, 0xe4, 0xf5, 0x06, 0x27}};
constexpr DefinitionId kA6b2SourceDefinition{801};
constexpr DefinitionId kA6b2LinkedDefinition{802};
constexpr DefinitionId kA6b2AssemblyRoot{803};
constexpr OccurrenceId kA6b2LeftOccurrence{901};
constexpr OccurrenceId kA6b2RightOccurrence{902};

Document a6b2LinkedDocument(double widthMm) {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  Document document;
  document.setBox({widthMm, 20.0, 10.0});
  auto& sketch = document.addSketch(SketchId{8100}, "A6b2 linked profile");
  sketch.geometry.addRectangle({0.0, 0.0}, {widthMm, 20.0});
  auto& body = document.addBody(8101, "A6b2 linked body");
  body.addFeature(std::make_unique<ExtrudeFeature>(
      FeatureId{8102}, SketchId{8100}, 10.0, "A6b2 linked solid"));
  CHECK(document.recompute());
  return document;
}

std::unique_ptr<ProductDocument> a6b2LinkedSource(double widthMm) {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            kA6b2SourceDefinition,
            std::make_unique<Document>(a6b2LinkedDocument(widthMm)))) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(
      std::move(definitions), kA6b2SourceDefinition,
      ProductDocumentIdentity{
          kA6b2SourceProject,
          {"A6b2 linked source", "2026-10-27T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<ProductDocument> a6b2LinkedAssembly(
    const LinkedPartBinding& binding) {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            kA6b2LinkedDefinition,
            std::make_unique<Document>(a6b2LinkedDocument(48.0)),
            binding)) == DefinitionInsertStatus::Inserted);
  const auto rightPlacement = RigidTransform3d::create(
      {55.0, 7.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(rightPlacement);
  AssemblyDefinition root(kA6b2AssemblyRoot);
  CHECK(root.insert(ComponentOccurrence(
            kA6b2LeftOccurrence, kA6b2LinkedDefinition,
            RigidTransform3d::identity(), {}, "A6b2 left")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kA6b2RightOccurrence, kA6b2LinkedDefinition,
            *rightPlacement.transform, {true, false, true},
            "A6b2 right")) == AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(
      std::move(definitions), kA6b2AssemblyRoot,
      ProductDocumentIdentity{
          kA6b2AssemblyProject,
          {"A6b2 linked assembly", "2026-10-27T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(bytes) == bytes.size() && file.flush();
}

struct A6b2OccurrenceFields final {
  DefinitionId definition;
  RigidTransform3d placement;
  OccurrenceFlags flags;
  std::string name;
  bool operator==(const A6b2OccurrenceFields&) const noexcept = default;
};

std::array<A6b2OccurrenceFields, 2> a6b2OccurrenceFields(
    const ProductDocument& product) {
  const auto* root = product.rootAssemblyDefinition();
  CHECK(root);
  const auto* left = root->find(kA6b2LeftOccurrence);
  const auto* right = root->find(kA6b2RightOccurrence);
  CHECK(left);
  CHECK(right);
  return {{{left->targetDefinitionId(), left->placement(), left->flags(),
            left->displayNameUtf8()},
           {right->targetDefinitionId(), right->placement(), right->flags(),
            right->displayNameUtf8()}}};
}

constexpr ProjectId kA6c1SourceProject{
    {0x51, 0x28, 0x35, 0x4a, 0x59, 0x6b, 0x4c, 0x8d,
     0x9e, 0xaf, 0xc1, 0xd2, 0xe3, 0xf4, 0x05, 0x17}};
constexpr ProjectId kA6c1AssemblyProject{
    {0x52, 0x39, 0x46, 0x5b, 0x6a, 0x7c, 0x4d, 0x9e,
     0xaf, 0xb0, 0xd2, 0xe3, 0xf4, 0x05, 0x16, 0x28}};
constexpr DefinitionId kA6c1SourceDefinition{811};
constexpr DefinitionId kA6c1LinkedDefinition{812};
constexpr DefinitionId kA6c1SharedAssembly{813};
constexpr DefinitionId kA6c1AssemblyRoot{814};
constexpr OccurrenceId kA6c1LeftPrefix{1};
constexpr OccurrenceId kA6c1RightPrefix{2};
constexpr OccurrenceId kA6c1LocalLeaf{7};

std::unique_ptr<ProductDocument> a6c1LinkedSource(double widthMm) {
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(
            kA6c1SourceDefinition,
            std::make_unique<Document>(a6b2LinkedDocument(widthMm)))) ==
        DefinitionInsertStatus::Inserted);
  auto created = ProductDocument::create(
      std::move(definitions), kA6c1SourceDefinition,
      ProductDocumentIdentity{
          kA6c1SourceProject,
          {"A6c1 linked source", "2026-10-29T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

std::unique_ptr<ProductDocument> a6c1NestedAssembly(
    const LinkedPartBinding& binding) {
  std::vector<PartDefinition> parts;
  parts.emplace_back(kA6c1LinkedDefinition,
                     std::make_unique<Document>(a6b2LinkedDocument(44.0)),
                     binding);

  const auto localPlacement = RigidTransform3d::create(
      {12.0, 3.0, 1.0}, {1.0, 0.0, 0.0, 0.0});
  const auto leftPlacement = RigidTransform3d::create(
      {25.0, -10.0, 0.0}, {0.7071067811865476, 0.0, 0.0,
                            0.7071067811865476});
  const auto rightPlacement = RigidTransform3d::create(
      {-35.0, 22.0, 4.0}, {0.7071067811865476, 0.0,
                           0.7071067811865476, 0.0});
  CHECK(localPlacement);
  CHECK(leftPlacement);
  CHECK(rightPlacement);

  AssemblyDefinition shared(kA6c1SharedAssembly);
  CHECK(shared.insert(ComponentOccurrence(
            kA6c1LocalLeaf, kA6c1LinkedDefinition,
            *localPlacement.transform, {}, "A6c1 shared leaf")) ==
        AssemblyOccurrenceInsertStatus::Inserted);

  AssemblyDefinition root(kA6c1AssemblyRoot);
  CHECK(root.insert(ComponentOccurrence(
            kA6c1LeftPrefix, kA6c1SharedAssembly, *leftPlacement.transform,
            {}, "A6c1 left prefix")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kA6c1RightPrefix, kA6c1SharedAssembly, *rightPlacement.transform,
            {}, "A6c1 right prefix")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  std::vector<AssemblyDefinition> assemblies;
  assemblies.push_back(std::move(shared));
  assemblies.push_back(std::move(root));
  auto created = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), kA6c1AssemblyRoot,
      ProductDocumentIdentity{
          kA6c1AssemblyProject,
          {"A6c1 recursive V8", "2026-10-29T00:00:00Z"}});
  CHECK(created);
  return std::move(created.product);
}

bool sameUploadState(const RendererResourceDiagnostics& left,
                     const RendererResourceDiagnostics& right) {
  if (left.sourceUploadAttempts != right.sourceUploadAttempts ||
      left.sourceUploadSuccesses != right.sourceUploadSuccesses ||
      left.sourceUploadFailures != right.sourceUploadFailures ||
      left.sourceDestroyCount != right.sourceDestroyCount ||
      left.sourcePruneCount != right.sourcePruneCount ||
      left.sources.size() != right.sources.size())
    return false;
  for (std::size_t index = 0; index < left.sources.size(); ++index) {
    if (left.sources[index].key != right.sources[index].key ||
        left.sources[index].uploadGeneration !=
            right.sources[index].uploadGeneration ||
        left.sources[index].contentFingerprint !=
            right.sources[index].contentFingerprint)
      return false;
  }
  return true;
}

test::A6c1GpuSmokeEvidence runA6c1V8GpuScenario(
    QApplication& application, AppSettings& settings,
    const QString& assemblyPath, const QString& sourcePath,
    const QByteArray& revisedSourceBytes) {
  test::A6c1GpuSmokeEvidence evidence;
  MainWindow window(settings);
  window.resize(960, 540);
  window.show();
  application.processEvents();

  auto* viewport = window.findChild<Viewport*>(QStringLiteral("mainViewport"));
  auto* tree =
      window.findChild<QTreeView*>(QStringLiteral("assemblyTreeView"));
  auto* isolation = window.findChild<QAction*>(
      QStringLiteral("assemblyIsolationOccurrenceAction"));
  auto* visibility = window.findChild<QAction*>(
      QStringLiteral("assemblyVisibilityOccurrenceAction"));
  auto* suppression = window.findChild<QAction*>(
      QStringLiteral("assemblySuppressionOccurrenceAction"));
  CHECK(viewport);
  CHECK(tree);
  CHECK(isolation);
  CHECK(visibility);
  CHECK(suppression);

  QString error;
  CHECK(window.loadProject(assemblyPath, &error));
  CHECK(error.isEmpty());
  application.processEvents();
  CHECK(MainWindowUndoTestAccess::persistenceFormat(window) ==
        project::ProjectPersistenceFormat::V8);
  CHECK(MainWindowUndoTestAccess::linkedRuntimeStatus(
            window, kA6c1LinkedDefinition) == LinkedPartRuntimeStatus::Exact);
  evidence.productionV8Open = true;

  const auto currentModel = [&]() {
    return qobject_cast<AssemblyTreeModel*>(tree->model());
  };
  const auto indexForPath = [&](std::initializer_list<OccurrenceId> ids) {
    auto* model = currentModel();
    if (!model) return QModelIndex{};
    const QModelIndex root = model->indexForDefinition(kA6c1AssemblyRoot);
    if (!root.isValid()) return QModelIndex{};
    QModelIndex parent = root;
    std::vector<OccurrenceId> expected;
    for (const OccurrenceId id : ids) {
      expected.push_back(id);
      QModelIndex found;
      for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex candidate = model->index(row, 0, parent);
        const auto address = model->addressForIndex(candidate);
        if (address && address->get().occurrence &&
            address->get().occurrence->occurrence.path.entries() == expected) {
          found = candidate;
          break;
        }
      }
      if (!found.isValid()) return QModelIndex{};
      parent = found;
    }
    return parent;
  };
  const auto selectPath = [&](std::initializer_list<OccurrenceId> ids) {
    const QModelIndex index = indexForPath(ids);
    if (!index.isValid()) return false;
    tree->setCurrentIndex(index);
    application.processEvents();
    return true;
  };
  const auto pathIs = [](const AssemblyOccurrenceIdentity& identity,
                         std::initializer_list<OccurrenceId> ids) {
    return identity.path.entries() == std::vector<OccurrenceId>(ids);
  };
  const auto terminalInstance = [&](std::initializer_list<OccurrenceId> ids)
      -> const AssemblyViewportSceneInstance* {
    const auto* session = viewport->assemblySession();
    if (!session || !session->scene()) return nullptr;
    for (const auto& instance : session->scene()->instances())
      if (pathIs(instance.occurrenceIdentity, ids)) return &instance;
    return nullptr;
  };

  auto* initialSession = const_cast<AssemblyViewportSession*>(
      viewport->assemblySession());
  CHECK(initialSession);
  CHECK(initialSession->scene());
  CHECK(initialSession->scene()->sources().size() == 1);
  CHECK(initialSession->scene()->instances().size() == 2);
  const auto* leftInstance =
      terminalInstance({kA6c1LeftPrefix, kA6c1LocalLeaf});
  const auto* rightInstance =
      terminalInstance({kA6c1RightPrefix, kA6c1LocalLeaf});
  CHECK(leftInstance);
  CHECK(rightInstance);
  CHECK(leftInstance->sourceKey == rightInstance->sourceKey);
  CHECK(leftInstance->mesh == rightInstance->mesh);
  evidence.sharedRecursivePrefixes =
      pathIs(leftInstance->occurrenceIdentity,
             {kA6c1LeftPrefix, kA6c1LocalLeaf}) &&
      pathIs(rightInstance->occurrenceIdentity,
             {kA6c1RightPrefix, kA6c1LocalLeaf}) &&
      leftInstance->occurrenceIdentity.path.entries().back() ==
          rightInstance->occurrenceIdentity.path.entries().back();
  evidence.oneSourceTwoInstances = true;

  const auto& product = MainWindowUndoTestAccess::product(window);
  const auto* root = product.rootAssemblyDefinition();
  const auto* shared = product.definitions().findAssembly(kA6c1SharedAssembly);
  CHECK(root);
  CHECK(shared);
  const auto* leftPrefix = root->find(kA6c1LeftPrefix);
  const auto* rightPrefix = root->find(kA6c1RightPrefix);
  const auto* localLeaf = shared->find(kA6c1LocalLeaf);
  CHECK(leftPrefix);
  CHECK(rightPrefix);
  CHECK(localLeaf);
  const auto expectedLeft = RigidTransform3d::compose(
      leftPrefix->placement(), localLeaf->placement());
  const auto expectedRight = RigidTransform3d::compose(
      rightPrefix->placement(), localLeaf->placement());
  const auto reversedLeft = RigidTransform3d::compose(
      localLeaf->placement(), leftPrefix->placement());
  CHECK(expectedLeft);
  CHECK(expectedRight);
  CHECK(reversedLeft);
  evidence.noncommutativeWorldTransforms =
      leftInstance->localToWorld == *expectedLeft.transform &&
      rightInstance->localToWorld == *expectedRight.transform &&
      *expectedLeft.transform != *reversedLeft.transform;

  CHECK(selectPath({kA6c1LeftPrefix, kA6c1LocalLeaf}));
  const auto leftSelection = viewport->selectedAssemblyOccurrenceView();
  CHECK(leftSelection);
  const AssemblyOccurrenceSelection rightSelection{
      initialSession->publicationHandle(), rightInstance->occurrenceIdentity};
  CHECK(initialSession->setHoveredOccurrence(rightSelection));
  viewport->repaint();
  application.processEvents();
  const auto rectangle = initialSession->picking().facesInRectangle(
      MainWindowUndoTestAccess::assemblyCamera(window), QPointF{0.0, 0.0},
      QPointF{static_cast<double>(std::max(1, viewport->width()) - 1),
              static_cast<double>(std::max(1, viewport->height()) - 1)});
  bool sharedLocalFaceUnderBothPrefixes = false;
  if (rectangle) {
    for (const auto& leftHit : rectangle.hits) {
      if (!pathIs(leftHit.address.occurrence,
                  {kA6c1LeftPrefix, kA6c1LocalLeaf}))
        continue;
      for (const auto& rightHit : rectangle.hits) {
        if (pathIs(rightHit.address.occurrence,
                   {kA6c1RightPrefix, kA6c1LocalLeaf}) &&
            leftHit.address.face == rightHit.address.face) {
          sharedLocalFaceUnderBothPrefixes = true;
          break;
        }
      }
      if (sharedLocalFaceUnderBothPrefixes) break;
    }
  }
  evidence.independentPickSelectionHoverHighlight =
      sharedLocalFaceUnderBothPrefixes && initialSession->selectedHighlight() &&
      initialSession->hoveredHighlight() &&
      pathIs(initialSession->selectedHighlight()->occurrence(),
             {kA6c1LeftPrefix, kA6c1LocalLeaf}) &&
      pathIs(initialSession->hoveredHighlight()->occurrence(),
             {kA6c1RightPrefix, kA6c1LocalLeaf});

  CHECK(isolation->isEnabled());
  const auto isolateUploads = viewport->rendererResourceDiagnostics();
  isolation->trigger();
  application.processEvents();
  evidence.terminalIsolation =
      viewport->assemblySession() == initialSession &&
      initialSession->isolationActive() &&
      initialSession->scene()->instances().size() == 1 &&
      initialSession->isolatedOccurrence() &&
      pathIs(*initialSession->isolatedOccurrence(),
             {kA6c1LeftPrefix, kA6c1LocalLeaf}) &&
      sameUploadState(isolateUploads,
                      viewport->rendererResourceDiagnostics());
  isolation->trigger();
  application.processEvents();
  CHECK(!initialSession->isolationActive());
  CHECK(initialSession->scene()->instances().size() == 2);

  CHECK(selectPath({kA6c1LeftPrefix}));
  evidence.intermediateIsolationDisabled = !isolation->isEnabled();
  CHECK(visibility->isEnabled());
  const auto beforeHide = viewport->rendererResourceDiagnostics();
  visibility->trigger();
  application.processEvents();
  const auto afterHide = viewport->rendererResourceDiagnostics();
  CHECK(viewport->assemblySession());
  CHECK(viewport->assemblySession()->scene()->instances().size() == 1);
  const bool hideNoUpload =
      afterHide.sourceUploadAttempts == beforeHide.sourceUploadAttempts &&
      afterHide.sourceUploadSuccesses == beforeHide.sourceUploadSuccesses;
  MainWindowUndoTestAccess::undo(window);
  application.processEvents();
  CHECK(selectPath({kA6c1LeftPrefix}));
  CHECK(suppression->isEnabled());
  const auto beforeSuppress = viewport->rendererResourceDiagnostics();
  suppression->trigger();
  application.processEvents();
  const auto afterSuppress = viewport->rendererResourceDiagnostics();
  CHECK(viewport->assemblySession());
  CHECK(viewport->assemblySession()->scene()->instances().size() == 1);
  const bool suppressNoUpload =
      afterSuppress.sourceUploadAttempts == beforeSuppress.sourceUploadAttempts &&
      afterSuppress.sourceUploadSuccesses ==
          beforeSuppress.sourceUploadSuccesses;
  MainWindowUndoTestAccess::undo(window);
  application.processEvents();
  evidence.ancestorHideSuppressNoUpload = hideNoUpload && suppressNoUpload;

  CHECK(selectPath({kA6c1LeftPrefix, kA6c1LocalLeaf}));
  const auto* beforeReloadSession = viewport->assemblySession();
  CHECK(beforeReloadSession);
  const auto beforeReloadPublication = beforeReloadSession->publicationHandle();
  QPointer<AssemblyTreeModel> beforeReloadTree(currentModel());
  std::weak_ptr<const AssemblyViewportScene> beforeReloadScene(
      beforeReloadSession->scene());
  std::weak_ptr<const BodyRenderMesh> beforeReloadMesh(
      beforeReloadSession->scene()->sources().front().mesh);
  const auto beforeReloadResources = viewport->rendererResourceDiagnostics();
  CHECK(writeBytes(sourcePath, revisedSourceBytes));
  MainWindowUndoTestAccess::reloadSelectedLinkedPart(window);
  application.processEvents();
  viewport->repaint();
  application.processEvents();
  const auto* reloadSession = viewport->assemblySession();
  CHECK(reloadSession);
  const auto reloadResources = viewport->rendererResourceDiagnostics();
  const auto reloadSelection = viewport->selectedAssemblyOccurrenceView();
  const bool reloadExact =
      reloadSession != beforeReloadSession && !beforeReloadPublication.isCurrent() &&
      beforeReloadTree.isNull() && beforeReloadScene.expired() &&
      beforeReloadMesh.expired() && reloadSelection &&
      pathIs(reloadSelection->occurrence,
             {kA6c1LeftPrefix, kA6c1LocalLeaf}) &&
      reloadSession->scene()->sources().size() == 1 &&
      reloadSession->scene()->instances().size() == 2 &&
      MainWindowUndoTestAccess::linkedRuntimeStatus(
          window, kA6c1LinkedDefinition) == LinkedPartRuntimeStatus::Exact;
  const bool reloadOneUpload =
      reloadResources.sourceUploadAttempts ==
          beforeReloadResources.sourceUploadAttempts + 1 &&
      reloadResources.sourceUploadSuccesses ==
          beforeReloadResources.sourceUploadSuccesses + 1;
  const bool reloadRetired =
      reloadResources.sourcePruneCount > beforeReloadResources.sourcePruneCount;

  const auto reloadPublication = reloadSession->publicationHandle();
  QPointer<AssemblyTreeModel> reloadTree(currentModel());
  std::weak_ptr<const AssemblyViewportScene> reloadScene(reloadSession->scene());
  std::weak_ptr<const BodyRenderMesh> reloadMesh(
      reloadSession->scene()->sources().front().mesh);
  CHECK(QFile::remove(sourcePath));
  MainWindowUndoTestAccess::undo(window);
  application.processEvents();
  viewport->repaint();
  application.processEvents();
  const auto* undoSession = viewport->assemblySession();
  CHECK(undoSession);
  const auto undoResources = viewport->rendererResourceDiagnostics();
  const auto undoSelection = viewport->selectedAssemblyOccurrenceView();
  const bool coldUndo =
      undoSession != reloadSession && !reloadPublication.isCurrent() &&
      reloadTree.isNull() && reloadScene.expired() && reloadMesh.expired() &&
      !QFile::exists(sourcePath) && undoSelection &&
      pathIs(undoSelection->occurrence,
             {kA6c1LeftPrefix, kA6c1LocalLeaf}) &&
      MainWindowUndoTestAccess::linkedRuntimeStatus(
          window, kA6c1LinkedDefinition) == LinkedPartRuntimeStatus::Unchecked;
  const bool undoOneUpload =
      undoResources.sourceUploadAttempts ==
          reloadResources.sourceUploadAttempts + 1 &&
      undoResources.sourceUploadSuccesses ==
          reloadResources.sourceUploadSuccesses + 1;
  const bool undoRetired =
      undoResources.sourcePruneCount > reloadResources.sourcePruneCount;

  const auto undoPublication = undoSession->publicationHandle();
  QPointer<AssemblyTreeModel> undoTree(currentModel());
  std::weak_ptr<const AssemblyViewportScene> undoScene(undoSession->scene());
  std::weak_ptr<const BodyRenderMesh> undoMesh(
      undoSession->scene()->sources().front().mesh);
  MainWindowUndoTestAccess::redo(window);
  application.processEvents();
  viewport->repaint();
  application.processEvents();
  const auto* redoSession = viewport->assemblySession();
  CHECK(redoSession);
  const auto redoResources = viewport->rendererResourceDiagnostics();
  const auto redoSelection = viewport->selectedAssemblyOccurrenceView();
  const bool coldRedo =
      redoSession != undoSession && !undoPublication.isCurrent() &&
      undoTree.isNull() && undoScene.expired() && undoMesh.expired() &&
      !QFile::exists(sourcePath) && redoSelection &&
      pathIs(redoSelection->occurrence,
             {kA6c1LeftPrefix, kA6c1LocalLeaf}) &&
      MainWindowUndoTestAccess::linkedRuntimeStatus(
          window, kA6c1LinkedDefinition) == LinkedPartRuntimeStatus::Unchecked;
  const bool redoOneUpload =
      redoResources.sourceUploadAttempts ==
          undoResources.sourceUploadAttempts + 1 &&
      redoResources.sourceUploadSuccesses ==
          undoResources.sourceUploadSuccesses + 1;
  const bool redoRetired =
      redoResources.sourcePruneCount > undoResources.sourcePruneCount;

  evidence.linkedReloadColdUndoRedo = reloadExact && coldUndo && coldRedo;
  evidence.oneUploadPerRevision =
      reloadOneUpload && undoOneUpload && redoOneUpload;
  evidence.oldResourcesRetired =
      reloadRetired && undoRetired && redoRetired;
  evidence.dpiIndependent = window.devicePixelRatioF() > 0.0;
  return evidence;
}

bool samePoint(const Point3d& left, const Point3d& right) {
  return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool groundedPlacementGuardEvidence() {
  ::solidar::detail::ScopedExplicitIdReservationPause pause;
  DefinitionRegistry definitions;
  if (definitions.insert(PartDefinition(
          DefinitionId{1}, std::make_unique<Document>())) !=
      DefinitionInsertStatus::Inserted)
    return false;
  AssemblyDefinition root(DefinitionId{100});
  if (root.insert(ComponentOccurrence(
          OccurrenceId{11}, DefinitionId{1},
          RigidTransform3d::identity(),
          {.visible = false, .suppressed = true, .grounded = false})) !=
          AssemblyOccurrenceInsertStatus::Inserted ||
      definitions.insert(std::move(root)) != DefinitionInsertStatus::Inserted)
    return false;
  auto made = ProductDocument::create(std::move(definitions), DefinitionId{100});
  if (!made) return false;
  auto path = OccurrencePath::create({OccurrenceId{11}});
  const auto moved = RigidTransform3d::create(
      {9.0, 2.0, -4.0}, {1.0, 0.0, 0.0, 0.0});
  if (!path || !moved) return false;
  auto ground = OccurrenceDelta::grounded(
      {made.product->incarnation(), made.product->rootDefinitionId(),
       *path.path, DefinitionId{1}},
      false, true);
  if (made.product->applyOccurrenceDelta(ground) !=
          OccurrenceDeltaStatus::Committed ||
      made.product->setRootOccurrencePlacement(*path.path,
                                               *moved.transform) !=
          ProductOccurrenceMutationStatus::OccurrenceGrounded)
    return false;
  const auto* occurrence = made.product->rootAssemblyDefinition()->find(
      OccurrenceId{11});
  if (!occurrence || occurrence->placement() != RigidTransform3d::identity() ||
      occurrence->flags().visible || !occurrence->flags().suppressed ||
      !occurrence->flags().grounded)
    return false;
  return made.product->revertOccurrenceDelta(ground) ==
             OccurrenceDeltaStatus::Committed &&
         made.product->setRootOccurrencePlacement(*path.path,
                                                  *moved.transform) ==
             ProductOccurrenceMutationStatus::Committed;
}

bool chooseSourceAndClick(QPushButton& button, const QString& path) {
  constexpr int kDialogPollIntervalMs = 10;
  // Cold CI starts can spend more than two seconds constructing Qt's widget
  // file dialog (plugin and directory model initialization).  Keep the
  // automation bounded, but do not turn that one-time startup into false
  // component-command failures.
  constexpr int kDialogTimeoutMs = 5000;

  bool inputAccepted = false;
  bool inputAttempted = false;
  bool timedOut = false;
  bool directoryRequested = false;
  bool fileSelectionRequested = false;
  int fileSelectionPolls = 0;
  QPointer<QFileDialog> targetDialog;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer chooser;
  QObject::connect(&chooser, &QTimer::timeout, &button, [&] {
    if (!inputAttempted) {
      auto* dialog = qobject_cast<QFileDialog*>(
          QApplication::activeModalWidget());
      if (!dialog) {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
          auto* candidate = qobject_cast<QFileDialog*>(widget);
          if (!candidate || !candidate->isVisible()) continue;
          dialog = candidate;
          break;
        }
      }
      if (dialog) {
        if (targetDialog != dialog) {
          targetDialog = dialog;
          directoryRequested = false;
          fileSelectionRequested = false;
          fileSelectionPolls = 0;
        }
        const QFileInfo source(path);
        const QString expectedDirectory = source.absolutePath();
        if (QDir::cleanPath(dialog->directory().absolutePath()) !=
            QDir::cleanPath(expectedDirectory)) {
          if (!directoryRequested) {
            dialog->setDirectory(expectedDirectory);
            directoryRequested = true;
          }
          return;
        }
        if (!fileSelectionRequested) {
          dialog->selectFile(source.fileName());
          if (auto* fileNameEdit =
                  dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit")))
            fileNameEdit->setText(source.fileName());
          fileSelectionRequested = true;
          fileSelectionPolls = 0;
        }
        const QString expected = source.absoluteFilePath();
        const QStringList selected = dialog->selectedFiles();
        inputAccepted = selected.size() == 1 &&
                        QFileInfo(selected.front()).absoluteFilePath() ==
                            expected;
        // QFileDialog's model can apply selectFile asynchronously.  Keep
        // polling until the exact requested path is observable; treating the
        // first not-yet-updated read as a completed attempt leaves the modal
        // dialog open until the fail-fast timeout.
        inputAttempted = inputAccepted;
        if (inputAccepted) {
          // This is deliberately the non-native Qt widget dialog in the smoke
          // process.  Accept synchronously once its public selectedFiles view
          // proves that the exact fixture path was entered; a queued accept can
          // lose a race with this helper's bounded timeout.
          chooser.stop();
          static_cast<QDialog*>(dialog)->accept();
          return;
        } else if (++fileSelectionPolls >= 25) {
          // QFileDialog may clear a pending selection when its asynchronous
          // directory model finishes.  Re-select only the file; resetting the
          // directory here would restart the model and can starve acceptance.
          dialog->selectFile(source.fileName());
          if (auto* fileNameEdit =
                  dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit")))
            fileNameEdit->setText(source.fileName());
          fileSelectionPolls = 0;
        }
      }
    }

    if (!inputAccepted && elapsed.elapsed() >= kDialogTimeoutMs) {
      timedOut = true;
      chooser.stop();
      for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* dialog = qobject_cast<QFileDialog*>(widget);
            dialog && dialog->isVisible()) {
          dialog->reject();
        }
      }
      if (auto* dialog =
              qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
        dialog->reject();
      }
    }
  });
  chooser.start(kDialogPollIntervalMs);
  button.click();
  chooser.stop();
  QApplication::processEvents();
  return inputAccepted && !timedOut;
}

bool runRenameDialog(QAction& action, const QString& name, bool accept,
                     const QAction* expectedDisabled = nullptr) {
  constexpr int kPollMs = 10;
  constexpr int kTimeoutMs = 2000;
  bool handled = false;
  bool disabledObserved = !expectedDisabled;
  bool timedOut = false;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &action, [&] {
    if (!handled) {
      if (auto* dialog = qobject_cast<QInputDialog*>(
              QApplication::activeModalWidget())) {
        disabledObserved =
            !expectedDisabled || !expectedDisabled->isEnabled();
        dialog->setTextValue(name);
        handled = true;
        if (accept)
          dialog->accept();
        else
          dialog->reject();
      }
    }
    if (elapsed.elapsed() >= kTimeoutMs) {
      timedOut = true;
      timer.stop();
      if (auto* dialog =
              qobject_cast<QDialog*>(QApplication::activeModalWidget()))
        dialog->reject();
    }
  });
  timer.start(kPollMs);
  action.trigger();
  timer.stop();
  QApplication::processEvents();
  return handled && disabledObserved && !timedOut;
}

bool runRenameFromTreeContextMenu(QTreeView& tree) {
  constexpr int kPollMs = 10;
  constexpr int kTimeoutMs = 2000;
  bool menuHandled = false;
  bool dialogHandled = false;
  bool timedOut = false;
  QPointer<QMenu> openedMenu;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &tree, [&] {
    if (!menuHandled) {
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
        for (QAction* action : menu->actions()) {
          if (action->text() !=
              QString::fromUtf8("Переименовать экземпляр…"))
            continue;
          openedMenu = menu;
          menuHandled = true;
          QTimer::singleShot(0, action, [action] { action->trigger(); });
          break;
        }
      }
    } else if (!dialogHandled) {
      if (auto* dialog = qobject_cast<QInputDialog*>(
              QApplication::activeModalWidget())) {
        dialogHandled = true;
        dialog->reject();
        if (openedMenu) openedMenu->close();
      }
    }
    if (elapsed.elapsed() >= kTimeoutMs) {
      timedOut = true;
      timer.stop();
      if (auto* dialog =
              qobject_cast<QDialog*>(QApplication::activeModalWidget()))
        dialog->reject();
      if (openedMenu) openedMenu->close();
    }
  });
  timer.start(kPollMs);
  const QModelIndex current = tree.currentIndex();
  const QPoint position = tree.visualRect(current).center();
  const bool invoked = current.isValid() &&
                       QMetaObject::invokeMethod(
                           &tree, "customContextMenuRequested",
                           Qt::DirectConnection, Q_ARG(QPoint, position));
  timer.stop();
  QApplication::processEvents();
  return invoked && menuHandled && dialogHandled && !timedOut;
}

bool treeContextMenuHasA5c2Actions(QTreeView& tree) {
  constexpr int kPollMs = 10;
  constexpr int kTimeoutMs = 2000;
  bool handled = false;
  bool timedOut = false;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &tree, [&] {
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
      bool visibility = false;
      bool isolation = false;
      for (const QAction* action : menu->actions()) {
        visibility = visibility ||
                     action->text() == QString::fromUtf8("Скрыть экземпляр") ||
                     action->text() == QString::fromUtf8("Показать экземпляр");
        isolation = isolation ||
                    action->text() ==
                        QString::fromUtf8("Изолировать экземпляр") ||
                    action->text() == QString::fromUtf8("Выйти из изоляции");
      }
      handled = visibility && isolation;
      menu->close();
      timer.stop();
      return;
    }
    if (elapsed.elapsed() >= kTimeoutMs) {
      timedOut = true;
      timer.stop();
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        menu->close();
    }
  });
  timer.start(kPollMs);
  const QModelIndex current = tree.currentIndex();
  const QPoint position = tree.visualRect(current).center();
  const bool invoked = current.isValid() &&
                       QMetaObject::invokeMethod(
                           &tree, "customContextMenuRequested",
                           Qt::DirectConnection, Q_ARG(QPoint, position));
  timer.stop();
  QApplication::processEvents();
  return invoked && handled && !timedOut;
}

bool treeContextMenuHasGroundingAction(QTreeView& tree) {
  constexpr int kPollMs = 10;
  constexpr int kTimeoutMs = 2000;
  bool handled = false;
  bool timedOut = false;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &tree, [&] {
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
      handled = std::any_of(menu->actions().begin(), menu->actions().end(),
                            [](const QAction* action) {
        return action->text() ==
                   QString::fromUtf8("Зафиксировать экземпляр") ||
               action->text() ==
                   QString::fromUtf8("Снять фиксацию экземпляра");
      });
      menu->close();
      timer.stop();
      return;
    }
    if (elapsed.elapsed() >= kTimeoutMs) {
      timedOut = true;
      timer.stop();
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        menu->close();
    }
  });
  timer.start(kPollMs);
  const QModelIndex current = tree.currentIndex();
  const QPoint position = tree.visualRect(current).center();
  const bool invoked = current.isValid() &&
                       QMetaObject::invokeMethod(
                           &tree, "customContextMenuRequested",
                           Qt::DirectConnection, Q_ARG(QPoint, position));
  timer.stop();
  QApplication::processEvents();
  return invoked && handled && !timedOut;
}

bool treeContextMenuHasSuppressionAction(QTreeView& tree) {
  constexpr int kPollMs = 10;
  constexpr int kTimeoutMs = 2000;
  bool handled = false;
  bool timedOut = false;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &tree, [&] {
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
      handled = std::any_of(menu->actions().begin(), menu->actions().end(),
                            [](const QAction* action) {
        return action->text() == QString::fromUtf8("Подавить экземпляр") ||
               action->text() ==
                   QString::fromUtf8("Снять подавление экземпляра");
      });
      menu->close();
      timer.stop();
      return;
    }
    if (elapsed.elapsed() >= kTimeoutMs) {
      timedOut = true;
      timer.stop();
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        menu->close();
    }
  });
  timer.start(kPollMs);
  const QModelIndex current = tree.currentIndex();
  const QPoint position = tree.visualRect(current).center();
  const bool invoked = current.isValid() &&
                       QMetaObject::invokeMethod(
                           &tree, "customContextMenuRequested",
                           Qt::DirectConnection, Q_ARG(QPoint, position));
  timer.stop();
  QApplication::processEvents();
  return invoked && handled && !timedOut;
}

bool treeContextMenuHasDuplicateAction(QTreeView& tree) {
  constexpr int kPollMs = 10;
  constexpr int kTimeoutMs = 2000;
  bool handled = false;
  bool timedOut = false;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &tree, [&] {
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
      handled = std::any_of(menu->actions().begin(), menu->actions().end(),
                            [](const QAction* action) {
        return action->text() ==
               QString::fromUtf8("Дублировать экземпляр");
      });
      menu->close();
      timer.stop();
      return;
    }
    if (elapsed.elapsed() >= kTimeoutMs) {
      timedOut = true;
      timer.stop();
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        menu->close();
    }
  });
  timer.start(kPollMs);
  const QModelIndex current = tree.currentIndex();
  const QPoint position = tree.visualRect(current).center();
  const bool invoked = current.isValid() &&
                       QMetaObject::invokeMethod(
                           &tree, "customContextMenuRequested",
                           Qt::DirectConnection, Q_ARG(QPoint, position));
  timer.stop();
  QApplication::processEvents();
  return invoked && handled && !timedOut;
}

bool treeContextMenuHasRemoveAction(QTreeView& tree) {
  constexpr int kPollMs = 10;
  constexpr int kTimeoutMs = 2000;
  bool handled = false;
  bool timedOut = false;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &tree, [&] {
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
      handled = std::any_of(menu->actions().begin(), menu->actions().end(),
                            [](const QAction* action) {
        return action->text() ==
               QString::fromUtf8("Удалить экземпляр");
      });
      menu->close();
      timer.stop();
      return;
    }
    if (elapsed.elapsed() >= kTimeoutMs) {
      timedOut = true;
      timer.stop();
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        menu->close();
    }
  });
  timer.start(kPollMs);
  const QModelIndex current = tree.currentIndex();
  const QPoint position = tree.visualRect(current).center();
  const bool invoked = current.isValid() &&
                       QMetaObject::invokeMethod(
                           &tree, "customContextMenuRequested",
                           Qt::DirectConnection, Q_ARG(QPoint, position));
  timer.stop();
  QApplication::processEvents();
  return invoked && handled && !timedOut;
}

bool definitionIsOrphan(QTreeView& tree, DefinitionId definitionId) {
  auto* model = qobject_cast<AssemblyTreeModel*>(tree.model());
  if (!model) return false;
  for (int row = 0; row < model->rowCount(); ++row) {
    const QModelIndex index = model->index(row, 0);
    const auto address = model->addressForIndex(index);
    if (address && address->get().kind == AssemblyTreeNodeKind::PartDefinition &&
        address->get().definitionId == definitionId)
      return model->data(index, kAssemblyTreeOrphanDefinitionRole).toBool() &&
             model->data(index).toString().contains(
                 QString::fromUtf8("не используется"));
  }
  return false;
}

int assemblyOccurrenceRows(QTreeView& tree) {
  if (!tree.model()) return -1;
  const QModelIndex root = tree.model()->index(0, 0);
  return root.isValid() ? tree.model()->rowCount(root) : -1;
}

struct SpatialProbePoint final {
  AssemblyOccurrenceIdentity occurrence;
  Point3d world;
};

test::A5c2SpatialConsumerEvidence probeSpatialConsumers(
    const AssemblyViewportSession& session,
    const std::vector<SpatialProbePoint>& expected,
    const std::vector<SpatialProbePoint>& excluded) {
  test::A5c2SpatialConsumerEvidence evidence;
  const auto& scene = session.scene();
  if (!scene || !scene->isCurrent() || !scene->worldBounds() ||
      expected.empty())
    return evidence;
  evidence.publicationCurrent =
      session.publicationHandle().isCurrent() &&
      scene->publicationHandle().isSameCurrentPublication(
          session.publicationHandle());
  constexpr QSize logicalSize{800, 600};
  const auto fit = fitAssemblyViewport(scene, logicalSize, 0.0F, 0.0F);
  if (!fit) return evidence;
  const ViewportCameraState camera{
      0.0F, 0.0F, fit.fit->zoom, fit.fit->pan, logicalSize, 1.0F,
      fit.fit->center, fit.fit->depthExtent};
  evidence.fitCurrentAndFiltered =
      fit.fit->generation == session.publicationHandle().generation() &&
      samePoint(fit.fit->center, scene->worldBounds()->center());

  bool faces = true;
  bool snaps = true;
  std::vector<AssemblyViewportSnapQueryResult> expectedSnaps;
  expectedSnaps.reserve(expected.size());
  for (const auto& point : expected) {
    const QPointF pixel = camera.worldToScreen(point.world);
    const auto face = session.faceAt(camera, pixel);
    faces = faces && face &&
            face.hit->address.occurrence == point.occurrence &&
            face.hit->publication.isSameCurrentPublication(
                session.publicationHandle());
    auto snap = session.picking().snapAt(camera, pixel, 12.0);
    snaps = snaps && snap && snap.hit->occurrence == point.occurrence &&
            snap.hit->publication.isSameCurrentPublication(
                session.publicationHandle());
    expectedSnaps.push_back(std::move(snap));
  }
  for (const auto& point : excluded) {
    const QPointF pixel = camera.worldToScreen(point.world);
    faces = faces &&
            session.faceAt(camera, pixel).status ==
                AssemblyViewportPickingQueryStatus::Miss;
    snaps = snaps &&
            session.picking().snapAt(camera, pixel, 12.0).status ==
                AssemblyViewportPickingQueryStatus::Miss;
  }
  evidence.faceCurrentAndFiltered = faces;
  evidence.snapCurrentAndFiltered = snaps;

  const auto rectangle = session.picking().facesInRectangle(
      camera, QPointF{0.0, 0.0}, QPointF{799.0, 599.0});
  bool rectangleValid = rectangle &&
      std::all_of(rectangle.hits.begin(), rectangle.hits.end(),
                  [&](const auto& hit) {
                    const bool expectedHit = std::any_of(
                        expected.begin(), expected.end(), [&](const auto& item) {
                          return item.occurrence == hit.address.occurrence;
                        });
                    return expectedHit &&
                           hit.publication.isSameCurrentPublication(
                               session.publicationHandle());
                  });
  for (const auto& item : expected) {
    rectangleValid = rectangleValid &&
        std::any_of(rectangle.hits.begin(), rectangle.hits.end(),
                    [&](const auto& hit) {
                      return hit.address.occurrence == item.occurrence;
                    });
  }
  evidence.rectangleCurrentAndFiltered = rectangleValid;

  AssemblyViewportRuler ruler;
  bool rulerValid = !expectedSnaps.empty() && expectedSnaps.front() &&
      ruler.commit(expectedSnaps.front()) ==
          AssemblyViewportRulerCommitStatus::AcceptedFirstPoint;
  if (rulerValid && expectedSnaps.size() > 1) {
    rulerValid = expectedSnaps[1] &&
        ruler.commit(expectedSnaps[1]) ==
            AssemblyViewportRulerCommitStatus::AcceptedSecondPoint;
  }
  ruler.validateCurrent(session.publicationHandle());
  rulerValid = rulerValid && ruler.firstPoint() &&
      ruler.firstPoint()->publication.isSameCurrentPublication(
          session.publicationHandle());
  if (expectedSnaps.size() > 1)
    rulerValid = rulerValid && ruler.secondPoint() && ruler.distance();
  evidence.rulerCurrentAndFiltered = rulerValid;
  return evidence;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication application(argc, argv);
  QTemporaryDir temporary(QDir::current().filePath(
      QStringLiteral("assembly-gpu-smoke-XXXXXX")));
  if (!temporary.isValid()) return EXIT_FAILURE;

  const QString partPath = temporary.filePath(QStringLiteral("part.solidar"));
  const QString sharedAssemblyPath =
      temporary.filePath(QStringLiteral("shared-source-assembly.solidar"));
  const QString definitionEditAssemblyPath =
      temporary.filePath(QStringLiteral("definition-edit-assembly.solidar"));
  const QString removeAssemblyPath =
      temporary.filePath(QStringLiteral("remove-assembly.solidar"));
  const QString a6b2Root =
      temporary.filePath(QStringLiteral("a6b2-linked"));
  const QString a6b2SourceDirectory =
      QDir(a6b2Root).filePath(QStringLiteral("parts"));
  const QString a6b2SourcePath =
      QDir(a6b2SourceDirectory).filePath(QStringLiteral("source.solidar"));
  const QString a6b2AssemblyPath =
      QDir(a6b2Root).filePath(QStringLiteral("assembly.solidar"));
  const QString a6c1Root =
      temporary.filePath(QStringLiteral("a6c1-recursive-v8"));
  const QString a6c1SourceDirectory =
      QDir(a6c1Root).filePath(QStringLiteral("parts"));
  const QString a6c1SourcePath =
      QDir(a6c1SourceDirectory).filePath(QStringLiteral("source.solidar"));
  const QString a6c1AssemblyPath =
      QDir(a6c1Root).filePath(QStringLiteral("assembly.solidar"));
  QString error;
  Document authoredPart;
  auto& authoredSketch = authoredPart.addSketch("A5b GPU smoke profile");
  authoredSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
  auto& authoredBody = authoredPart.addBody("A5b GPU smoke body");
  authoredBody.addFeature(std::make_unique<ExtrudeFeature>(
      authoredSketch.id, 5.0, "A5b GPU smoke box"));
  if (!authoredPart.recompute()) return EXIT_FAILURE;
  Document definitionEditPart = [&authoredPart] {
    ::solidar::detail::ScopedExplicitIdReservationPause pause;
    return Document{authoredPart};
  }();
  Document removePart = [&authoredPart] {
    ::solidar::detail::ScopedExplicitIdReservationPause pause;
    return Document{authoredPart};
  }();
  CHECK(QDir().mkpath(a6b2SourceDirectory));
  CHECK(QDir().mkpath(a6c1SourceDirectory));
  auto a6b2InitialSource = a6b2LinkedSource(48.0);
  const auto a6b2InitialEncoded = project::encodeProductEnvelopeV5Root(
      *a6b2InitialSource, kA6b2SourceProject,
      {QStringLiteral("A6b2 linked source"),
       QStringLiteral("2026-10-27T00:00:00Z")});
  CHECK(a6b2InitialEncoded.succeeded());
  const LinkedPartBinding a6b2Binding{
      {kA6b2SourceProject, kA6b2SourceDefinition},
      "parts/source.solidar",
      project::linkedSourceSha256(
          QByteArrayView(a6b2InitialEncoded.payload->bytes))};
  auto a6b2Assembly = a6b2LinkedAssembly(a6b2Binding);
  const auto a6b2AssemblyEncoded = project::encodeProductEnvelopeV7Root(
      *a6b2Assembly, kA6b2AssemblyProject,
      {QStringLiteral("A6b2 linked assembly"),
       QStringLiteral("2026-10-27T00:00:00Z")});
  CHECK(a6b2AssemblyEncoded.succeeded());
  auto a6b2RevisedSource = a6b2LinkedSource(72.0);
  const auto a6b2RevisedEncoded = project::encodeProductEnvelopeV5Root(
      *a6b2RevisedSource, kA6b2SourceProject,
      {QStringLiteral("A6b2 linked source"),
       QStringLiteral("2026-10-27T00:00:00Z")});
  CHECK(a6b2RevisedEncoded.succeeded());
  auto a6c1InitialSource = a6c1LinkedSource(44.0);
  const auto a6c1InitialEncoded = project::encodeProductEnvelopeV5Root(
      *a6c1InitialSource, kA6c1SourceProject,
      {QStringLiteral("A6c1 linked source"),
       QStringLiteral("2026-10-29T00:00:00Z")});
  CHECK(a6c1InitialEncoded.succeeded());
  const LinkedPartBinding a6c1Binding{
      {kA6c1SourceProject, kA6c1SourceDefinition},
      "parts/source.solidar",
      project::linkedSourceSha256(
          QByteArrayView(a6c1InitialEncoded.payload->bytes))};
  auto a6c1Assembly = a6c1NestedAssembly(a6c1Binding);
  const auto a6c1AssemblyEncoded = project::encodeProductEnvelopeV8Root(
      *a6c1Assembly, kA6c1AssemblyProject,
      {QStringLiteral("A6c1 recursive V8"),
       QStringLiteral("2026-10-29T00:00:00Z")});
  CHECK(a6c1AssemblyEncoded.succeeded());
  auto a6c1RevisedSource = a6c1LinkedSource(68.0);
  const auto a6c1RevisedEncoded = project::encodeProductEnvelopeV5Root(
      *a6c1RevisedSource, kA6c1SourceProject,
      {QStringLiteral("A6c1 linked source"),
       QStringLiteral("2026-10-29T00:00:00Z")});
  CHECK(a6c1RevisedEncoded.succeeded());
  if (!project::ProjectFile::saveDocument(
          partPath, authoredPart,
          {project::ProjectPersistenceFormat::V4}, &error) ||
      !writeDefinitionEditAssemblyFixture(definitionEditAssemblyPath,
                                          std::move(definitionEditPart)) ||
      !writeRemoveAssemblyFixture(removeAssemblyPath,
                                  std::move(removePart)) ||
      !writeSharedSourceAssemblyFixture(sharedAssemblyPath,
                                        std::move(authoredPart)) ||
      !writeBytes(a6b2SourcePath, a6b2InitialEncoded.payload->bytes) ||
      !writeBytes(a6b2AssemblyPath, a6b2AssemblyEncoded.payload->bytes) ||
      !writeBytes(a6c1SourcePath, a6c1InitialEncoded.payload->bytes) ||
      !writeBytes(a6c1AssemblyPath, a6c1AssemblyEncoded.payload->bytes))
    return EXIT_FAILURE;
  const QString assemblyPath =
      QString::fromUtf8(SOLIDAR_MINIMAL_V6_FIXTURE_PATH);
  AppSettings settings(temporary.filePath(QStringLiteral("settings.ini")));
  MainWindow window(settings);
  window.resize(960, 540);
  const QString platformName = QGuiApplication::platformName().toLower();
  const bool headlessPlatform =
      platformName.contains(QStringLiteral("offscreen")) ||
      platformName.contains(QStringLiteral("minimal"));
  // Showing an offscreen top-level window is still required for QWidget
  // visibility/layout assertions and modal automation.  It does not turn an
  // offscreen/software context into native GPU evidence; that remains guarded
  // independently by classifyGpuSmokeRuntime below.
  window.show();
  application.processEvents();

  auto* viewport = window.findChild<Viewport*>(QStringLiteral("mainViewport"));
  auto* tree =
      window.findChild<QTreeView*>(QStringLiteral("assemblyTreeView"));
  auto* history = window.findChild<QDockWidget*>(QStringLiteral("historyDock"));
  auto* ribbon =
      window.findChild<QWidget*>(QStringLiteral("assemblyReadOnlyRibbon"));
  auto* modelRibbon = window.findChild<ModelRibbon*>();
  if (!viewport || !tree || !history || !ribbon || !modelRibbon)
    return EXIT_FAILURE;

  const bool partLoaded = window.loadProject(partPath, &error);
  const bool partHadNoAssembly = partLoaded && !viewport->assemblySession();
  error.clear();
  const bool assemblyLoaded = window.loadProject(assemblyPath, &error);
  application.processEvents();
  auto* model = qobject_cast<AssemblyTreeModel*>(tree->model());
  const auto* firstSession = viewport->assemblySession();
  const bool openReadOnly =
      assemblyLoaded && error.isEmpty() && model && firstSession &&
      ribbon->isVisible() && !modelRibbon->isEnabled();
  const bool completeFirstCommit =
      firstSession && firstSession->epoch().isValid() && firstSession->scene() &&
      firstSession->scene()->isCurrent();
  const bool partAssemblySwitch = partHadNoAssembly && openReadOnly;

  QModelIndex firstOccurrence;
  QModelIndex secondOccurrence;
  std::optional<AssemblyOccurrenceSelection> firstIdentity;
  std::optional<AssemblyOccurrenceSelection> secondIdentity;
  if (model) {
    const QModelIndex root = model->index(0, 0);
    firstOccurrence = model->index(0, 0, root);
    secondOccurrence = model->index(1, 0, root);
    if (const auto address = model->addressForIndex(firstOccurrence))
      firstIdentity = address->get().occurrence;
    if (const auto address = model->addressForIndex(secondOccurrence))
      secondIdentity = address->get().occurrence;
  }

  if (firstOccurrence.isValid()) tree->setCurrentIndex(firstOccurrence);
  application.processEvents();
  const bool treeToViewport =
      firstIdentity &&
      viewport->selectedAssemblyOccurrenceView() == firstIdentity;

  bool viewportToTree = false;
  bool hiddenSuppressed = false;
  if (secondIdentity && viewport->setSelectedAssemblyOccurrence(*secondIdentity,
                                                                 true)) {
    application.processEvents();
    viewportToTree = tree->currentIndex() == secondOccurrence;
    const auto* session = viewport->assemblySession();
    const QString hiddenLabel = model->data(secondOccurrence).toString();
    hiddenSuppressed =
        session && !session->selectedHighlight() &&
        hiddenLabel.contains(QString::fromUtf8("скрытый")) &&
        hiddenLabel.contains(QString::fromUtf8("подавлен"));
  }

  const auto firstEpoch = firstSession ? firstSession->epoch()
                                       : ProductPresentationEpoch{};
  error.clear();
  const bool secondAssemblyLoaded = window.loadProject(assemblyPath, &error);
  application.processEvents();
  const auto* secondSession = viewport->assemblySession();
  const bool assemblyAssemblySwitch =
      secondAssemblyLoaded && error.isEmpty() && secondSession &&
      secondSession->epoch().isValid() && secondSession->epoch() != firstEpoch &&
      !viewport->selectedAssemblyOccurrenceView();

  const bool timelineHidden =
      !history->isVisible() && disabled(window, "toggleHistoryAction");
  const bool noMutationCommands =
      disabled(window, "createProjectAction") &&
      disabled(window, "saveProjectAction") &&
      disabled(window, "saveProjectAsAction") &&
      disabled(window, "importStepAction") &&
      disabled(window, "exportStepAction") &&
      disabled(window, "exportStlAction") && disabled(window, "undoAction") &&
      disabled(window, "redoAction") && !modelRibbon->isEnabled() &&
      window.findChild<QToolButton*>(
          QStringLiteral("assemblyDisplayModeCommand")) &&
      window.findChild<QToolButton*>(
          QStringLiteral("assemblyDisplayModeCommand"))->isEnabled();

  bool layoutMatrix = true;
  const std::array<QSize, 4> logicalSizes{
      QSize{1920, 1080}, QSize{1536, 864}, QSize{1280, 720}, QSize{960, 540}};
  for (const QSize size : logicalSizes) {
    window.resize(size);
    application.processEvents();
    const auto* label = ribbon->findChild<QLabel*>();
    layoutMatrix = layoutMatrix && label && label->isVisible() &&
                   label->sizeHint().width() <= ribbon->width() &&
                   tree->isVisible() && tree->viewport()->width() > 0;
  }
  auto* newAssembly = window.findChild<QPushButton*>(
      QStringLiteral("assemblyNewCommand"));
  auto* insert = window.findChild<QPushButton*>(
      QStringLiteral("assemblyInsertCommand"));
  auto* apply = window.findChild<QPushButton*>(
      QStringLiteral("assemblyApplyCommand"));
  auto* cancel = window.findChild<QPushButton*>(
      QStringLiteral("assemblyCancelCommand"));
  auto* undo = window.findChild<QAction*>(QStringLiteral("undoAction"));
  auto* redo = window.findChild<QAction*>(QStringLiteral("redoAction"));
  auto* rename = window.findChild<QAction*>(
      QStringLiteral("assemblyRenameOccurrenceAction"));
  auto* renameButton = window.findChild<QPushButton*>(
      QStringLiteral("assemblyRenameOccurrenceCommand"));
  auto* renameShortcut = window.findChild<QShortcut*>(
      QStringLiteral("assemblyRenameOccurrenceShortcut"));
  auto* visibility = window.findChild<QAction*>(
      QStringLiteral("assemblyVisibilityOccurrenceAction"));
  auto* visibilityButton = window.findChild<QPushButton*>(
      QStringLiteral("assemblyVisibilityOccurrenceCommand"));
  auto* visibilityShortcut = window.findChild<QShortcut*>(
      QStringLiteral("assemblyVisibilityOccurrenceShortcut"));
  auto* isolation = window.findChild<QAction*>(
      QStringLiteral("assemblyIsolationOccurrenceAction"));
  auto* isolationButton = window.findChild<QPushButton*>(
      QStringLiteral("assemblyIsolationOccurrenceCommand"));
  auto* grounding = window.findChild<QAction*>(
      QStringLiteral("assemblyGroundingOccurrenceAction"));
  auto* groundingButton = window.findChild<QPushButton*>(
      QStringLiteral("assemblyGroundingOccurrenceCommand"));
  auto* groundingShortcut = window.findChild<QShortcut*>(
      QStringLiteral("assemblyGroundingOccurrenceShortcut"));
  auto* suppression = window.findChild<QAction*>(
      QStringLiteral("assemblySuppressionOccurrenceAction"));
  auto* suppressionButton = window.findChild<QPushButton*>(
      QStringLiteral("assemblySuppressionOccurrenceCommand"));
  auto* suppressionShortcut = window.findChild<QShortcut*>(
      QStringLiteral("assemblySuppressionOccurrenceShortcut"));
  auto* duplicate = window.findChild<QAction*>(
      QStringLiteral("assemblyDuplicateOccurrenceAction"));
  auto* duplicateButton = window.findChild<QPushButton*>(
      QStringLiteral("assemblyDuplicateOccurrenceCommand"));
  auto* duplicateShortcut = window.findChild<QShortcut*>(
      QStringLiteral("assemblyDuplicateOccurrenceShortcut"));
  auto* remove = window.findChild<QAction*>(
      QStringLiteral("assemblyRemoveOccurrenceAction"));
  auto* removeButton = window.findChild<QPushButton*>(
      QStringLiteral("assemblyRemoveOccurrenceCommand"));
  auto* removeShortcut = window.findChild<QShortcut*>(
      QStringLiteral("assemblyRemoveOccurrenceShortcut"));
  auto* editDefinition = window.findChild<QAction*>(
      QStringLiteral("assemblyEditDefinitionAction"));
  auto* editDefinitionButton = window.findChild<QPushButton*>(
      QStringLiteral("assemblyEditDefinitionCommand"));
  auto* definitionBreadcrumb = window.findChild<QLabel*>(
      QStringLiteral("assemblyDefinitionEditBreadcrumb"));
  auto* definitionBack = window.findChild<QPushButton*>(
      QStringLiteral("assemblyDefinitionEditBackCommand"));
  auto* definitionMove = window.findChild<QToolButton*>(
      QStringLiteral("moveCommand"));
  auto* definitionMoveX = window.findChild<QDoubleSpinBox*>(
      QStringLiteral("moveXSpin"));
  auto* definitionMoveY = window.findChild<QDoubleSpinBox*>(
      QStringLiteral("moveYSpin"));
  auto* definitionMoveZ = window.findChild<QDoubleSpinBox*>(
      QStringLiteral("moveZSpin"));
  auto* definitionMoveApply = window.findChild<QPushButton*>(
      QStringLiteral("moveAcceptButton"));
  auto* definitionMoveCancel = window.findChild<QPushButton*>(
      QStringLiteral("moveCancelButton"));
  bool a5c2LayoutMatrix = visibilityButton && isolationButton;
  bool a5c3LayoutMatrix = groundingButton;
  bool a5c4LayoutMatrix = suppressionButton;
  bool a5c5LayoutMatrix = duplicateButton;
  bool a5c6LayoutMatrix = removeButton;
  bool a5dLayoutMatrix = editDefinitionButton && definitionBack &&
                         definitionBreadcrumb && definitionMove &&
                         definitionMoveX && definitionMoveY &&
                         definitionMoveZ && definitionMoveApply &&
                         definitionMoveCancel;
  for (const QSize size : logicalSizes) {
    window.resize(size);
    application.processEvents();
    const bool logicalControls = visibilityButton->sizeHint().width() > 0 &&
                                 isolationButton->sizeHint().width() > 0 &&
                                 visibilityButton->sizeHint().height() > 0 &&
                                 isolationButton->sizeHint().height() > 0;
    const bool visibleLayout =
        ribbon->width() > 0 && ribbon->height() > 0 &&
        visibilityButton->sizeHint().width() <= ribbon->width() &&
        isolationButton->sizeHint().width() <= ribbon->width() &&
        visibilityButton->geometry().isValid() &&
        isolationButton->geometry().isValid() &&
        visibilityButton->isVisible() && isolationButton->isVisible();
    a5c2LayoutMatrix = a5c2LayoutMatrix && logicalControls &&
                       (headlessPlatform || visibleLayout);
    const bool groundingLayout =
        groundingButton && groundingButton->sizeHint().width() > 0 &&
        groundingButton->sizeHint().height() > 0 &&
        groundingButton->sizeHint().width() <= ribbon->width() &&
        groundingButton->geometry().isValid() &&
        (headlessPlatform || groundingButton->isVisible());
    a5c3LayoutMatrix = a5c3LayoutMatrix && groundingLayout;
    const bool suppressionLayout =
        suppressionButton && suppressionButton->sizeHint().width() > 0 &&
        suppressionButton->sizeHint().height() > 0 &&
        suppressionButton->sizeHint().width() <= ribbon->width() &&
        suppressionButton->geometry().isValid() &&
        (headlessPlatform || suppressionButton->isVisible());
    a5c4LayoutMatrix = a5c4LayoutMatrix && suppressionLayout;
    const bool duplicateLayout =
        duplicateButton && duplicateButton->sizeHint().width() > 0 &&
        duplicateButton->sizeHint().height() > 0 &&
        duplicateButton->sizeHint().width() <= ribbon->width() &&
        duplicateButton->geometry().isValid() &&
        (headlessPlatform || duplicateButton->isVisible());
    a5c5LayoutMatrix = a5c5LayoutMatrix && duplicateLayout;
    const bool removeLayout =
        removeButton && removeButton->sizeHint().width() > 0 &&
        removeButton->sizeHint().height() > 0 &&
        removeButton->sizeHint().width() <= ribbon->width() &&
        removeButton->geometry().isValid() &&
        (headlessPlatform || removeButton->isVisible());
    a5c6LayoutMatrix = a5c6LayoutMatrix && removeLayout;
    const bool definitionEditLayout =
        editDefinitionButton && editDefinitionButton->sizeHint().width() > 0 &&
        editDefinitionButton->sizeHint().height() > 0 &&
        editDefinitionButton->sizeHint().width() <= ribbon->width() &&
        editDefinitionButton->geometry().isValid() &&
        (headlessPlatform || editDefinitionButton->isVisible());
    a5dLayoutMatrix = a5dLayoutMatrix && definitionEditLayout;
  }
  bool a5bNewAssembly = false;
  bool insertGhost = false;
  bool moveRotateGhost = false;
  bool cancelExactRestore = false;
  bool failedStagePreservesPreview = false;
  bool failedOpenPreservesPreview = false;
  bool applyOneUndo = false;
  bool undoRedoStableIds = false;
  bool selectionPublicationRefresh = false;
  bool replacementTeardownOrder = false;
  bool a5c1Entrypoints = false;
  bool a5c1ApplyCancel = false;
  bool a5c1UndoRedo = false;
  bool a5c1OnlyName = false;
  bool a5c1Disabled = rename && renameButton && renameShortcut &&
                      !rename->isEnabled() && !renameButton->isEnabled() &&
                      !renameShortcut->isEnabled();
  bool a5c1SelectionPublicationStable = false;
  bool a5c1NoUpload = false;
  bool a5c1ReplacementTeardown = false;
  bool a5c2Entrypoints = false;
  bool a5c2VisibilityUndoRedo = false;
  bool a5c2OnlyVisible = false;
  bool a5c2HiddenSpatialExcluded = false;
  bool a5c2SharedSourceNoRemeshUpload = false;
  bool a5c2LastUserPruneReacquire = false;
  bool a5c2IsolateShowAll = false;
  bool a5c2IsolatePresentationOnly = false;
  bool a5c2IsolateNoUploadPrune = false;
  bool a5c2ReplacementTeardown = false;
  bool a5c3Entrypoints = false;
  bool a5c3GroundUnground = false;
  bool a5c3UndoRedo = false;
  bool a5c3OnlyGrounded = false;
  const bool a5c3PlacementGuard = groundedPlacementGuardEvidence();
  bool a5c3VisibilitySuppressionIndependent = false;
  bool a5c3SelectionPublicationRefresh = false;
  bool a5c3NoRemeshUpload = false;
  bool a5c3InsertChain = false;
  bool a5c3IsolateDisabled = false;
  bool a5c3ReplacementTeardown = false;
  bool a5c4Entrypoints = false;
  bool a5c4SuppressUnsuppress = false;
  bool a5c4UndoRedo = false;
  bool a5c4OnlySuppressed = false;
  bool a5c4VisibilityGroundedIndependent = false;
  bool a5c4SuppressedSpatialExcluded = false;
  bool a5c4SharedSourceNoRemeshUpload = false;
  bool a5c4LastUserPruneReacquire = false;
  bool a5c4SelectionPublicationRefresh = false;
  bool a5c4InsertChain = false;
  bool a5c4IsolateDisabled = false;
  bool a5c4ReplacementTeardown = false;
  bool a5c5Entrypoints = false;
  bool a5c5NewStableOccurrenceId = false;
  bool a5c5SameDefinitionDocumentBrep = false;
  bool a5c5ExactPlacementNameFlags = false;
  bool a5c5SelectionApplyUndoRedo = false;
  bool a5c5HiddenSuppressedGrounded = false;
  bool a5c5SameEpochPublicationRefresh = false;
  bool a5c5SharedSourceNoRemeshUpload = false;
  bool a5c5InstanceDraw = false;
  bool a5c5RectanglePickingBoth = false;
  bool a5c5TopologyReuse = false;
  bool a5c5InsertDisabled = false;
  bool a5c5RenameDisabled = false;
  bool a5c5IsolateDisabled = false;
  bool a5c5FailureAtomicity = false;
  bool a5c5ReplacementTeardown = false;
  bool a5c6Entrypoints = false;
  bool a5c6RemoveUndoRedo = false;
  bool a5c6OnlyOccurrenceNode = false;
  bool a5c6OrphanDefinitionRetained = false;
  bool a5c6OrphanTreeFeedback = false;
  bool a5c6LastOccurrenceEmptyRoot = false;
  bool a5c6HiddenSuppressedGrounded = false;
  bool a5c6SelectionClearRestore = false;
  bool a5c6SameEpochPublicationRefresh = false;
  bool a5c6SharedSourceNoRemeshUpload = false;
  bool a5c6LastUserPruneReacquire = false;
  bool a5c6InstanceDrawPicking = false;
  bool a5c6UndoCollisionRollback = false;
  bool a5c6MixedChain = false;
  bool a5c6EmptyCamera = false;
  bool a5c6TopologyPruneReacquire = false;
  bool a5c6IsolateDisabled = false;
  bool a5c6FailureAtomicity = false;
  bool a5c6ReplacementTeardown = false;
  bool a5dEntryReturnBreadcrumb = false;
  bool a5dOneOwnerOneHistory = false;
  bool a5dSharedEditAllOccurrences = false;
  bool a5dIdsPlacementsFlagsStable = false;
  bool a5dFailedRecomputeRollback = false;
  bool a5dFailedProjectionRollback = false;
  bool a5dUndoRedoShared = false;
  bool a5dToolReplacementTeardown = false;
  bool a5dEditedSourceOneUpload = false;
  bool a5dOtherSourcesReused = false;
  bool a5dHiddenSuppressedStable = false;
  bool a5dSelectionCameraRestore = false;
  bool a5dOrphanDefinitionEdit = false;

  if (newAssembly && insert && apply && cancel && undo && redo && rename &&
      renameButton && renameShortcut && visibility && visibilityButton &&
      visibilityShortcut && isolation && isolationButton && grounding &&
      groundingButton && groundingShortcut && suppression &&
      suppressionButton && suppressionShortcut && duplicate &&
      duplicateButton && duplicateShortcut && remove && removeButton &&
      removeShortcut) {
    newAssembly->click();
    application.processEvents();
    a5bNewAssembly = viewport->assemblySession() &&
                     assemblyOccurrenceRows(*tree) == 0 &&
                     window.windowFilePath().isEmpty() &&
                     !window.isWindowModified();

    const bool chose = chooseSourceAndClick(*insert, partPath);
    const auto meshBuilds =
        viewport->assemblyPlacementPreview().meshBuildCount();
    insertGhost = chose && viewport->assemblyPlacementPreview().active() &&
                  assemblyOccurrenceRows(*tree) == 0 &&
                  !window.isWindowModified();
    a5c5InsertDisabled = insertGhost && !duplicate->isEnabled() &&
                         !duplicateButton->isEnabled() &&
                         !duplicateShortcut->isEnabled();
    auto* tx = window.findChild<QDoubleSpinBox*>(
        QStringLiteral("assemblyTranslation0"));
    auto* rz = window.findChild<QDoubleSpinBox*>(
        QStringLiteral("assemblyRotation2"));
    if (tx && rz) {
      tx->setValue(25.0);
      rz->setValue(90.0);
      application.processEvents();
      moveRotateGhost = viewport->assemblyPlacementPreview().active() &&
                        viewport->assemblyPlacementPreview().meshBuildCount() ==
                            meshBuilds &&
                        viewport->assemblyPlacementPreview()
                                .placementUpdateCount() >= 2;
    }

    const QString corruptStagePath =
        temporary.filePath(QStringLiteral("corrupt-source.solidar"));
    QFile corruptStage(corruptStagePath);
    const bool corruptWritten = corruptStage.open(QIODevice::WriteOnly) &&
                                corruptStage.write("not-json") == 8;
    corruptStage.close();
    const auto previewUpdates =
        viewport->assemblyPlacementPreview().placementUpdateCount();
    const auto previewToken = viewport->assemblyPlacementPreview().token();
    const bool choseCorrupt =
        corruptWritten && chooseSourceAndClick(*insert, corruptStagePath);
    failedStagePreservesPreview =
        choseCorrupt &&
        viewport->assemblyPlacementPreview().active() &&
        viewport->assemblyPlacementPreview().token() == previewToken &&
        viewport->assemblyPlacementPreview().meshBuildCount() == meshBuilds &&
        viewport->assemblyPlacementPreview().placementUpdateCount() ==
            previewUpdates;

    const auto previewEpoch = viewport->assemblySession()->epoch();
    error.clear();
    const bool failedOpen = !window.loadProject(
        temporary.filePath(QStringLiteral("missing-open.solidar")), &error);
    failedOpenPreservesPreview =
        failedOpen && viewport->assemblyPlacementPreview().active() &&
        viewport->assemblySession()->epoch() == previewEpoch &&
        assemblyOccurrenceRows(*tree) == 0;

    cancel->click();
    application.processEvents();
    cancelExactRestore = !viewport->assemblyPlacementPreview().active() &&
                         assemblyOccurrenceRows(*tree) == 0 &&
                         !window.isWindowModified() && !undo->isEnabled();

    const bool secondSourceChosen = chooseSourceAndClick(*insert, partPath);
    if (secondSourceChosen && tx && rz) {
      tx->setValue(31.0);
      rz->setValue(47.0);
      apply->click();
      application.processEvents();
      auto* appliedModel = qobject_cast<AssemblyTreeModel*>(tree->model());
      const QModelIndex appliedRoot =
          appliedModel ? appliedModel->index(0, 0) : QModelIndex{};
      const QModelIndex appliedOccurrence =
          appliedModel ? appliedModel->index(0, 0, appliedRoot)
                       : QModelIndex{};
      const auto appliedAddress = appliedModel
                                      ? appliedModel->addressForIndex(
                                            appliedOccurrence)
                                      : AssemblyTreeModel::AddressView{};
      const std::optional<OccurrencePath> appliedPath =
          appliedAddress && appliedAddress->get().occurrence
              ? std::optional<OccurrencePath>(
                    appliedAddress->get().occurrence->occurrence.path)
              : std::nullopt;
      const auto appliedTransform =
          viewport->assemblySession() && viewport->assemblySession()->scene() &&
                  !viewport->assemblySession()->scene()->instances().empty()
              ? std::optional<RigidTransform3d>(
                    viewport->assemblySession()->scene()->instances().front()
                        .localToWorld)
              : std::nullopt;
      const auto appliedEpoch = viewport->assemblySession()->epoch();
      const bool exactlyOneApplied =
          assemblyOccurrenceRows(*tree) == 1 && undo->isEnabled() &&
          !redo->isEnabled() && window.isWindowModified() &&
          !viewport->assemblyPlacementPreview().active();
      undo->trigger();
      application.processEvents();
      applyOneUndo = exactlyOneApplied &&
                     assemblyOccurrenceRows(*tree) == 0 &&
                     !undo->isEnabled() && redo->isEnabled() &&
                     !window.isWindowModified();
      redo->trigger();
      application.processEvents();
      auto* redoneModel = qobject_cast<AssemblyTreeModel*>(tree->model());
      const QModelIndex redoneRoot =
          redoneModel ? redoneModel->index(0, 0) : QModelIndex{};
      const QModelIndex redoneOccurrence =
          redoneModel ? redoneModel->index(0, 0, redoneRoot) : QModelIndex{};
      const auto redoneAddress =
          redoneModel ? redoneModel->addressForIndex(redoneOccurrence)
                      : AssemblyTreeModel::AddressView{};
      const auto* redoneSession = viewport->assemblySession();
      undoRedoStableIds =
          appliedPath && redoneAddress && redoneAddress->get().occurrence &&
          *appliedPath ==
              redoneAddress->get().occurrence->occurrence.path &&
          appliedTransform && redoneSession && redoneSession->scene() &&
          !redoneSession->scene()->instances().empty() &&
          *appliedTransform ==
              redoneSession->scene()->instances().front().localToWorld;
      selectionPublicationRefresh =
          redoneSession && redoneSession->epoch() != appliedEpoch &&
          viewport->selectedAssemblyOccurrenceView() &&
          viewport->selectedAssemblyOccurrenceView()->isCurrentFor(
              redoneSession->publicationHandle());

      a5c1Entrypoints = rename->isEnabled() && renameButton->isEnabled() &&
                        renameShortcut->isEnabled() &&
                        runRenameFromTreeContextMenu(*tree);
      const auto* renameSessionBefore = viewport->assemblySession();
      const auto* renameSceneBefore =
          renameSessionBefore ? renameSessionBefore->scene().get() : nullptr;
      const auto renameEpochBefore =
          renameSessionBefore ? renameSessionBefore->epoch()
                              : ProductPresentationEpoch{};
      const auto renamePublicationBefore =
          renameSessionBefore
              ? renameSessionBefore->publicationHandle().generation()
              : AssemblyScenePublicationGeneration{};
      const auto renameSelectionBefore =
          viewport->selectedAssemblyOccurrenceView();
      const auto renameUploadsBefore =
          viewport->rendererResourceDiagnostics();
      const bool renameApplied =
          runRenameDialog(*rename, QString::fromUtf8("Опора 🔩"), true,
                          duplicate);
      a5c5RenameDisabled = renameApplied;
      auto* renamedModel = qobject_cast<AssemblyTreeModel*>(tree->model());
      const QModelIndex renamedRoot =
          renamedModel ? renamedModel->index(0, 0) : QModelIndex{};
      const QModelIndex renamedOccurrence =
          renamedModel ? renamedModel->index(0, 0, renamedRoot)
                       : QModelIndex{};
      const bool named =
          renamedModel &&
          renamedModel->data(renamedOccurrence).toString().contains(
              QString::fromUtf8("Опора 🔩"));
      const bool exactRuntimeAfterApply =
          viewport->assemblySession() == renameSessionBefore &&
          viewport->assemblySession()->scene().get() == renameSceneBefore &&
          viewport->assemblySession()->epoch() == renameEpochBefore &&
          viewport->assemblySession()->publicationHandle().generation() ==
              renamePublicationBefore &&
          viewport->selectedAssemblyOccurrenceView() == renameSelectionBefore;
      const bool renameCancelled =
          runRenameDialog(*rename, QString::fromUtf8("Не применять"), false);
      const bool cancelKeptName =
          qobject_cast<AssemblyTreeModel*>(tree->model())
              ->data(tree->currentIndex())
              .toString()
              .contains(QString::fromUtf8("Опора 🔩"));
      a5c1ApplyCancel = renameApplied && named && renameCancelled &&
                        cancelKeptName;
      a5c1OnlyName = renameApplied && named && exactRuntimeAfterApply;
      a5c1SelectionPublicationStable = exactRuntimeAfterApply;
      a5c1NoUpload =
          exactRuntimeAfterApply &&
          sameUploadState(renameUploadsBefore,
                          viewport->rendererResourceDiagnostics());

      undo->trigger();
      application.processEvents();
      const bool renameUndone =
          viewport->assemblySession() == renameSessionBefore &&
          viewport->assemblySession()->scene().get() == renameSceneBefore &&
          !qobject_cast<AssemblyTreeModel*>(tree->model())
               ->data(tree->currentIndex())
               .toString()
               .contains(QString::fromUtf8("Опора 🔩"));
      const bool undoDidNotUpload = sameUploadState(
          renameUploadsBefore, viewport->rendererResourceDiagnostics());
      redo->trigger();
      application.processEvents();
      const bool renameRedone =
          viewport->assemblySession() == renameSessionBefore &&
          viewport->assemblySession()->scene().get() == renameSceneBefore &&
          qobject_cast<AssemblyTreeModel*>(tree->model())
              ->data(tree->currentIndex())
              .toString()
              .contains(QString::fromUtf8("Опора 🔩"));
      a5c1UndoRedo = renameUndone && renameRedone;
      a5c1NoUpload =
          a5c1NoUpload && undoDidNotUpload &&
          sameUploadState(renameUploadsBefore,
                          viewport->rendererResourceDiagnostics());

      // A5c3 begins on the real Insert->Rename chain. Grounding must publish
      // a fresh same-epoch occurrence view, preserve every geometric field
      // and renderer resource, and keep the semantic selection rebound.
      a5c3Entrypoints =
          grounding->isEnabled() && groundingButton->isEnabled() &&
          groundingShortcut->isEnabled() &&
          groundingShortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_G) &&
          treeContextMenuHasGroundingAction(*tree);
      const auto* groundingBeforeSession = viewport->assemblySession();
      const auto groundingBeforePublication =
          groundingBeforeSession
              ? groundingBeforeSession->publicationHandle()
              : AssemblyScenePublicationHandle{};
      const auto groundingBeforeEpoch =
          groundingBeforeSession ? groundingBeforeSession->epoch()
                                 : ProductPresentationEpoch{};
      const auto groundingBeforeSelection =
          viewport->selectedAssemblyOccurrenceView();
      const auto groundingBeforeUploads =
          viewport->rendererResourceDiagnostics();
      const auto groundingBeforeInstance =
          groundingBeforeSession && groundingBeforeSession->scene() &&
                  !groundingBeforeSession->scene()->instances().empty()
              ? std::optional<AssemblyViewportSceneInstance>(
                    groundingBeforeSession->scene()->instances().front())
              : std::nullopt;
      grounding->trigger();
      application.processEvents();
      const auto* groundedSession = viewport->assemblySession();
      const auto groundedSelection =
          viewport->selectedAssemblyOccurrenceView();
      const auto groundedIndex = tree->currentIndex();
      const bool groundedRole =
          groundedIndex.isValid() && tree->model()->data(
              groundedIndex, kAssemblyTreeAuthoredGroundedRole).toBool();
      const auto groundedState = groundedSession && groundedSelection
          ? std::find_if(groundedSession->occurrences().begin(),
                         groundedSession->occurrences().end(),
                         [&](const auto& state) {
                           return state.identity ==
                                  groundedSelection->occurrence;
                         })
          : std::vector<AssemblyOccurrenceSessionState>::const_iterator{};
      const bool groundedStateFound =
          groundedSession && groundedSelection &&
          groundedState != groundedSession->occurrences().end();
      const auto groundedInstance =
          groundedSession && groundedSession->scene() &&
                  !groundedSession->scene()->instances().empty()
              ? std::optional<AssemblyViewportSceneInstance>(
                    groundedSession->scene()->instances().front())
              : std::nullopt;
      const bool geometryUnchanged =
          groundingBeforeInstance && groundedInstance &&
          groundingBeforeInstance->sourceKey == groundedInstance->sourceKey &&
          groundingBeforeInstance->mesh == groundedInstance->mesh &&
          groundingBeforeInstance->localToWorld ==
              groundedInstance->localToWorld &&
          groundingBeforeInstance->occurrenceIdentity ==
              groundedInstance->occurrenceIdentity &&
          groundingBeforeInstance->worldBounds.minimum().x ==
              groundedInstance->worldBounds.minimum().x &&
          groundingBeforeInstance->worldBounds.minimum().y ==
              groundedInstance->worldBounds.minimum().y &&
          groundingBeforeInstance->worldBounds.minimum().z ==
              groundedInstance->worldBounds.minimum().z &&
          groundingBeforeInstance->worldBounds.maximum().x ==
              groundedInstance->worldBounds.maximum().x &&
          groundingBeforeInstance->worldBounds.maximum().y ==
              groundedInstance->worldBounds.maximum().y &&
          groundingBeforeInstance->worldBounds.maximum().z ==
              groundedInstance->worldBounds.maximum().z;
      a5c3OnlyGrounded =
          groundedStateFound && groundedState->grounded &&
          groundedState->visible && !groundedState->suppressed &&
          groundingBeforeInstance && !groundingBeforeInstance->grounded &&
          groundedInstance && groundedInstance->grounded && geometryUnchanged;
      a5c3SelectionPublicationRefresh =
          groundedSession && groundingBeforeSession != groundedSession &&
          groundedSession->epoch() == groundingBeforeEpoch &&
          !groundedSession->publicationHandle().isSameCurrentPublication(
              groundingBeforePublication) &&
          groundedSelection && groundingBeforeSelection &&
          groundedSelection->occurrence ==
              groundingBeforeSelection->occurrence &&
          groundedSelection->isCurrentFor(
              groundedSession->publicationHandle()) &&
          groundedSession->selectedHighlight();
      a5c3NoRemeshUpload =
          geometryUnchanged && groundedSession &&
          groundedSession->sceneDiagnostics().meshBuildAttempts == 0 &&
          (headlessPlatform ||
           sameUploadState(groundingBeforeUploads,
                           viewport->rendererResourceDiagnostics()));
      a5c3GroundUnground =
          groundedRole &&
          grounding->text() == QString::fromUtf8("Снять фиксацию экземпляра");
      undo->trigger();
      application.processEvents();
      const bool groundUndone =
          !tree->model()
               ->data(tree->currentIndex(),
                      kAssemblyTreeAuthoredGroundedRole)
               .toBool();
      redo->trigger();
      application.processEvents();
      const bool groundRedone =
          tree->model()
              ->data(tree->currentIndex(), kAssemblyTreeAuthoredGroundedRole)
              .toBool();
      a5c3UndoRedo = groundUndone && groundRedone;
      a5c3InsertChain =
          a5c3UndoRedo && tree->model()->data(tree->currentIndex()).toString()
                                .contains(QString::fromUtf8("Опора 🔩"));
      a5c3NoRemeshUpload =
          a5c3NoRemeshUpload &&
          (headlessPlatform ||
           sameUploadState(groundingBeforeUploads,
                           viewport->rendererResourceDiagnostics()));

      // A5c4 starts on the real Insert->Rename->Ground chain.  Shift+Space is
      // deliberately distinct from Space, and both ribbon and context route
      // through the same suppression action.
      a5c4Entrypoints =
          suppression->isEnabled() && suppressionButton->isEnabled() &&
          suppressionShortcut->isEnabled() &&
          suppressionShortcut->key() ==
              QKeySequence(Qt::SHIFT | Qt::Key_Space) &&
          suppressionShortcut->key() != visibilityShortcut->key() &&
          treeContextMenuHasSuppressionAction(*tree);
      const bool chainVisibleBefore =
          tree->model()
              ->data(tree->currentIndex(), kAssemblyTreeAuthoredVisibleRole)
              .toBool();
      const bool chainGroundedBefore =
          tree->model()
              ->data(tree->currentIndex(), kAssemblyTreeAuthoredGroundedRole)
              .toBool();
      suppression->trigger();
      application.processEvents();
      const bool chainSuppressed =
          tree->model()
              ->data(tree->currentIndex(), kAssemblyTreeAuthoredSuppressedRole)
              .toBool();
      const bool chainNamePreserved =
          tree->model()->data(tree->currentIndex()).toString().contains(
              QString::fromUtf8("Опора 🔩"));
      const bool chainOtherBitsPreserved =
          tree->model()
                  ->data(tree->currentIndex(),
                         kAssemblyTreeAuthoredVisibleRole)
                  .toBool() == chainVisibleBefore &&
          tree->model()
                  ->data(tree->currentIndex(),
                         kAssemblyTreeAuthoredGroundedRole)
                  .toBool() == chainGroundedBefore;
      const bool suppressionTextChanged =
          suppression->text() ==
          QString::fromUtf8("Снять подавление экземпляра");
      const bool shortcutInvoked = QMetaObject::invokeMethod(
          suppressionShortcut, "activated", Qt::DirectConnection);
      application.processEvents();
      const bool chainUnsuppressed =
          !tree->model()
               ->data(tree->currentIndex(),
                      kAssemblyTreeAuthoredSuppressedRole)
               .toBool();
      a5c4SuppressUnsuppress = chainSuppressed && suppressionTextChanged &&
                               shortcutInvoked && chainUnsuppressed;
      a5c4InsertChain = a5c4SuppressUnsuppress && chainNamePreserved &&
                        chainOtherBitsPreserved;

      // A5c5 duplicates the selected occurrence structurally while retaining
      // the exact definition-owned Document/B-Rep and render source.
      [&] {
      a5c5Entrypoints =
          duplicate->isEnabled() && duplicateButton->isEnabled() &&
          duplicateShortcut->isEnabled() &&
          duplicateShortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_D) &&
          treeContextMenuHasDuplicateAction(*tree);
      const auto beforeDuplicateSelection =
          viewport->selectedAssemblyOccurrenceView();
      const auto* beforeDuplicateSession = viewport->assemblySession();
      const auto beforeDuplicatePublication =
          beforeDuplicateSession->publicationHandle();
      // Establish an exact native frame baseline before the structural
      // change. Offscreen still exercises the functional evidence below but
      // may not claim submitted/drawn GPU counters.
      viewport->repaint();
      application.processEvents();
      const auto beforeDuplicateResources =
          viewport->rendererResourceDiagnostics();
      const auto beforeDuplicateCount = assemblyOccurrenceRows(*tree);
      const auto beforeDuplicateHistorySize =
          MainWindowUndoTestAccess::history(window).size();
      const auto beforeDuplicateHistoryCursor =
          MainWindowUndoTestAccess::history(window).cursor();
      const auto beforeDuplicateHistoryBytes =
          MainWindowUndoTestAccess::history(window).retainedBytes();
      const auto beforeDuplicateRevision =
          MainWindowUndoTestAccess::history(window).currentRevision();
      MainWindowUndoTestAccess::injectProjection(
          window, AssemblyViewportSessionFailurePoint::Presentation);
      duplicate->trigger();
      application.processEvents();
      MainWindowUndoTestAccess::injectProjection(
          window, AssemblyViewportSessionFailurePoint::None);
      a5c5FailureAtomicity =
          assemblyOccurrenceRows(*tree) == beforeDuplicateCount &&
          MainWindowUndoTestAccess::history(window).size() ==
              beforeDuplicateHistorySize &&
          MainWindowUndoTestAccess::history(window).cursor() ==
              beforeDuplicateHistoryCursor &&
          MainWindowUndoTestAccess::history(window).retainedBytes() ==
              beforeDuplicateHistoryBytes &&
          MainWindowUndoTestAccess::history(window).currentRevision() ==
              beforeDuplicateRevision &&
          viewport->assemblySession() == beforeDuplicateSession &&
          viewport->assemblySession()->publicationHandle()
              .isSameCurrentPublication(beforeDuplicatePublication) &&
          viewport->selectedAssemblyOccurrenceView() ==
              beforeDuplicateSelection &&
          sameUploadState(beforeDuplicateResources,
                          viewport->rendererResourceDiagnostics());

      const auto& duplicateProduct =
          MainWindowUndoTestAccess::product(window);
      const auto expectedDuplicateId =
          duplicateProduct.lowestFreeRootOccurrenceId();
      const auto* duplicateRoot = duplicateProduct.rootAssemblyDefinition();
      const auto sourceId =
          beforeDuplicateSelection
              ? beforeDuplicateSelection->occurrence.path.entries().front()
              : OccurrenceId{};
      const auto* sourceOccurrence =
          duplicateRoot ? duplicateRoot->find(sourceId) : nullptr;
      const auto sourceTarget = sourceOccurrence
          ? sourceOccurrence->targetDefinitionId()
          : DefinitionId{};
      const auto sourcePlacement = sourceOccurrence
          ? sourceOccurrence->placement()
          : RigidTransform3d::identity();
      const auto sourceFlags = sourceOccurrence
          ? sourceOccurrence->flags()
          : OccurrenceFlags{};
      const std::string sourceName = sourceOccurrence
          ? sourceOccurrence->displayNameUtf8()
          : std::string{};
      const auto* sourcePart =
          duplicateProduct.definitions().findPart(sourceTarget);
      const auto* sourceDocument = sourcePart ? &sourcePart->document() : nullptr;
      const TopoDS_Shape* sourceBrep =
          sourceDocument && !sourceDocument->bodies().empty()
              ? sourceDocument->bodies().front().resultShape().get()
              : nullptr;
      const auto duplicateBeforeEpoch = beforeDuplicateSession->epoch();
      const auto duplicateBeforeSceneInstances =
          beforeDuplicateSession->sceneDiagnostics().activeInstances;
      const auto duplicateBeforePickingInstances =
          beforeDuplicateSession->picking().diagnostics().activeInstances;
      const auto duplicateBeforeTopologySources =
          beforeDuplicateSession->picking().diagnostics()
              .residentTopologySources;
      duplicate->trigger();
      application.processEvents();
      viewport->repaint();
      application.processEvents();
      const auto duplicateSelection =
          viewport->selectedAssemblyOccurrenceView();
      const auto duplicateId =
          duplicateSelection
              ? std::optional<OccurrenceId>(
                    duplicateSelection->occurrence.path.entries().front())
              : std::nullopt;
      const auto* duplicatedOccurrence =
          duplicateId
              ? MainWindowUndoTestAccess::product(window)
                    .rootAssemblyDefinition()
                    ->find(*duplicateId)
              : nullptr;
      const auto* duplicatedPart = duplicatedOccurrence
          ? MainWindowUndoTestAccess::product(window).definitions().findPart(
                duplicatedOccurrence->targetDefinitionId())
          : nullptr;
      const TopoDS_Shape* duplicatedBrep =
          duplicatedPart && !duplicatedPart->document().bodies().empty()
              ? duplicatedPart->document().bodies().front().resultShape().get()
              : nullptr;
      const auto* duplicatedSession = viewport->assemblySession();
      const auto afterDuplicateResources =
          viewport->rendererResourceDiagnostics();
      a5c5NewStableOccurrenceId =
          expectedDuplicateId && duplicateId &&
          *duplicateId == *expectedDuplicateId && *duplicateId != sourceId;
      a5c5SameDefinitionDocumentBrep =
          duplicatedOccurrence && duplicatedPart == sourcePart &&
          &duplicatedPart->document() == sourceDocument &&
          duplicatedBrep == sourceBrep;
      a5c5ExactPlacementNameFlags =
          duplicatedOccurrence &&
          duplicatedOccurrence->targetDefinitionId() == sourceTarget &&
          duplicatedOccurrence->placement() == sourcePlacement &&
          duplicatedOccurrence->flags() == sourceFlags &&
          duplicatedOccurrence->displayNameUtf8() == sourceName;
      a5c5SameEpochPublicationRefresh =
          duplicatedSession && duplicatedSession != beforeDuplicateSession &&
          duplicatedSession->epoch() == duplicateBeforeEpoch &&
          !duplicatedSession->publicationHandle().isSameCurrentPublication(
              beforeDuplicatePublication) &&
          duplicateSelection && duplicateSelection->isCurrentFor(
                                    duplicatedSession->publicationHandle());
      a5c5SharedSourceNoRemeshUpload =
          duplicatedSession &&
          duplicatedSession->sceneDiagnostics().meshBuildAttempts == 0 &&
          (headlessPlatform ||
           sameUploadState(beforeDuplicateResources,
                           afterDuplicateResources));
      a5c5InstanceDraw =
          duplicatedSession &&
          duplicatedSession->sceneDiagnostics().activeInstances ==
              duplicateBeforeSceneInstances + 1 &&
          duplicatedSession->picking().diagnostics().activeInstances ==
              duplicateBeforePickingInstances + 1 &&
          duplicatedSession->selectedHighlight() &&
          assemblyOccurrenceRows(*tree) == beforeDuplicateCount + 1 &&
          (headlessPlatform ||
           (beforeDuplicateResources.submittedAssemblyInstances ==
                duplicateBeforeSceneInstances &&
            beforeDuplicateResources.drawnAssemblyInstances ==
                duplicateBeforeSceneInstances &&
            afterDuplicateResources.submittedAssemblyInstances ==
                beforeDuplicateResources.submittedAssemblyInstances + 1 &&
            afterDuplicateResources.drawnAssemblyInstances ==
                beforeDuplicateResources.drawnAssemblyInstances + 1));

      // Coincident duplicates are intentionally ambiguous for a single ray.
      // A full-view rectangle is a real topology-backed picking consumer and
      // must return both distinct current-publication occurrence identities.
      const auto duplicateRectangle = duplicatedSession
          ? duplicatedSession->picking().facesInRectangle(
                MainWindowUndoTestAccess::assemblyCamera(window),
                QPointF{0.0, 0.0},
                QPointF{static_cast<double>(std::max(1, viewport->width()) - 1),
                        static_cast<double>(std::max(1, viewport->height()) - 1)})
          : AssemblyFaceRectangleQueryResult{};
      const bool duplicateRectangleCurrent =
          duplicatedSession && beforeDuplicateSelection &&
          duplicateRectangle &&
          std::all_of(duplicateRectangle.hits.begin(),
                      duplicateRectangle.hits.end(), [&](const auto& hit) {
                        return hit.publication.isSameCurrentPublication(
                            duplicatedSession->publicationHandle());
                      });
      const bool rectangleHasSource =
          duplicateRectangleCurrent &&
          std::any_of(duplicateRectangle.hits.begin(),
                      duplicateRectangle.hits.end(), [&](const auto& hit) {
                        return hit.address.occurrence ==
                               beforeDuplicateSelection->occurrence;
                      });
      const bool rectangleHasDuplicate =
          duplicateRectangleCurrent && duplicateSelection &&
          std::any_of(duplicateRectangle.hits.begin(),
                      duplicateRectangle.hits.end(), [&](const auto& hit) {
                        return hit.address.occurrence ==
                               duplicateSelection->occurrence;
                      });
      a5c5RectanglePickingBoth =
          rectangleHasSource && rectangleHasDuplicate;
      a5c5TopologyReuse =
          duplicatedSession &&
          duplicatedSession->picking().diagnostics()
                  .topologySourceBuildAttempts == 0 &&
          duplicatedSession->picking().diagnostics()
                  .topologySourceBuildSuccesses == 0 &&
          duplicatedSession->picking().diagnostics().topologySourceReuses ==
              duplicatedSession->scene()->sources().size() &&
          duplicatedSession->picking().diagnostics()
                  .residentTopologySources == duplicateBeforeTopologySources &&
          rectangleHasSource && rectangleHasDuplicate;

      undo->trigger();
      application.processEvents();
      const auto undoDuplicateSelection =
          viewport->selectedAssemblyOccurrenceView();
      const bool duplicateUndone =
          assemblyOccurrenceRows(*tree) == beforeDuplicateCount &&
          undoDuplicateSelection &&
          undoDuplicateSelection->occurrence.path.entries().front() == sourceId;
      redo->trigger();
      application.processEvents();
      const auto redoDuplicateSelection =
          viewport->selectedAssemblyOccurrenceView();
      const bool duplicateRedone =
          duplicateId && assemblyOccurrenceRows(*tree) ==
                             beforeDuplicateCount + 1 &&
          redoDuplicateSelection &&
          redoDuplicateSelection->occurrence.path.entries().front() ==
              *duplicateId;
      undo->trigger();
      application.processEvents();
      a5c5SelectionApplyUndoRedo =
          duplicateSelection && duplicateUndone && duplicateRedone &&
          viewport->selectedAssemblyOccurrenceView() &&
          viewport->selectedAssemblyOccurrenceView()
                  ->occurrence.path.entries().front() == sourceId;

      // Hidden/suppressed/grounded sources remain eligible. The structural
      // copy retains all authored bits and therefore adds no rendered item.
      visibility->trigger();
      application.processEvents();
      suppression->trigger();
      application.processEvents();
      const auto hiddenDuplicateInstanceCount =
          viewport->assemblySession()->sceneDiagnostics().activeInstances;
      duplicate->trigger();
      application.processEvents();
      const auto hiddenDuplicateSelection =
          viewport->selectedAssemblyOccurrenceView();
      const auto* hiddenDuplicate = hiddenDuplicateSelection
          ? MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->find(hiddenDuplicateSelection->occurrence.path.entries()
                           .front())
          : nullptr;
      a5c5HiddenSuppressedGrounded =
          hiddenDuplicate && !hiddenDuplicate->flags().visible &&
          hiddenDuplicate->flags().suppressed &&
          hiddenDuplicate->flags().grounded &&
          viewport->assemblySession()->sceneDiagnostics().activeInstances ==
              hiddenDuplicateInstanceCount &&
          !viewport->assemblySession()->selectedHighlight();
      }();

      // Dedicated real unified-history sequence. This is intentionally one
      // command stream, not a conjunction of earlier independent markers:
      // Insert -> Duplicate -> Rename -> Visibility -> Ground -> Suppress ->
      // Remove, followed by every Undo and Redo boundary.
      // Earlier smoke evidence intentionally dirtied its disposable project;
      // suppress only that harness confirmation so this bounded run never
      // waits in an unrelated QMessageBox.
      window.setWindowModified(false);
      newAssembly->click();
      application.processEvents();
      bool mixedChain = chooseSourceAndClick(*insert, partPath);
      if (mixedChain) {
        std::vector<AssemblyCommandRevision> mixedRevisions;
        apply->click();
        application.processEvents();
        mixedRevisions.push_back(
            MainWindowUndoTestAccess::history(window).currentRevision());
        const auto insertedSelection =
            viewport->selectedAssemblyOccurrenceView();
        const auto insertedId = insertedSelection
            ? insertedSelection->occurrence.path.entries().front()
            : OccurrenceId{};
        const auto* insertedOccurrence =
            MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->find(insertedId);
        const auto insertedTarget = insertedOccurrence
            ? insertedOccurrence->targetDefinitionId()
            : DefinitionId{};
        const auto insertedPlacement = insertedOccurrence
            ? insertedOccurrence->placement()
            : RigidTransform3d::identity();
        duplicate->trigger();
        application.processEvents();
        mixedRevisions.push_back(
            MainWindowUndoTestAccess::history(window).currentRevision());
        const auto duplicatedSelection =
            viewport->selectedAssemblyOccurrenceView();
        const auto duplicatedId = duplicatedSelection
            ? duplicatedSelection->occurrence.path.entries().front()
            : OccurrenceId{};
        const bool duplicated = insertedId.isValid() && duplicatedId.isValid() &&
                                insertedId != duplicatedId;
        const bool renamed = runRenameDialog(
            *rename, QString::fromUtf8("Полная цепочка 🔩"), true);
        mixedRevisions.push_back(
            MainWindowUndoTestAccess::history(window).currentRevision());
        visibility->trigger();
        application.processEvents();
        mixedRevisions.push_back(
            MainWindowUndoTestAccess::history(window).currentRevision());
        grounding->trigger();
        application.processEvents();
        mixedRevisions.push_back(
            MainWindowUndoTestAccess::history(window).currentRevision());
        suppression->trigger();
        application.processEvents();
        mixedRevisions.push_back(
            MainWindowUndoTestAccess::history(window).currentRevision());
        const auto* edited = MainWindowUndoTestAccess::product(window)
                                 .rootAssemblyDefinition()
                                 ->find(duplicatedId);
        const bool exactFields =
            edited && edited->displayNameUtf8() == "Полная цепочка 🔩" &&
            !edited->flags().visible && edited->flags().grounded &&
            edited->flags().suppressed;
        remove->trigger();
        application.processEvents();
        mixedRevisions.push_back(
            MainWindowUndoTestAccess::history(window).currentRevision());
        const auto& mixedHistory = MainWindowUndoTestAccess::history(window);
        const bool removedAtBoundary =
            mixedHistory.size() == 7 && mixedHistory.cursor() == 7 &&
            assemblyOccurrenceRows(*tree) == 1 &&
            MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->find(insertedId) &&
            !MainWindowUndoTestAccess::product(window)
                 .rootAssemblyDefinition()
                 ->find(duplicatedId) &&
            !viewport->selectedAssemblyOccurrenceView();

        undo->trigger();
        application.processEvents();
        const auto* restored = MainWindowUndoTestAccess::product(window)
                                   .rootAssemblyDefinition()
                                   ->find(duplicatedId);
        const bool undoRemoveExact =
            restored && restored->displayNameUtf8() == "Полная цепочка 🔩" &&
            !restored->flags().visible && restored->flags().grounded &&
            restored->flags().suppressed &&
            viewport->selectedAssemblyOccurrenceView() &&
            viewport->selectedAssemblyOccurrenceView()
                    ->occurrence.path.entries().front() == duplicatedId;
        for (int step = 0; step < 5; ++step) {
          undo->trigger();
          application.processEvents();
        }
        const bool duplicateUndoBoundary =
            MainWindowUndoTestAccess::history(window).cursor() == 1 &&
            assemblyOccurrenceRows(*tree) == 1 &&
            MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->find(insertedId) &&
            !MainWindowUndoTestAccess::product(window)
                 .rootAssemblyDefinition()
                 ->find(duplicatedId) &&
            viewport->selectedAssemblyOccurrenceView() &&
            viewport->selectedAssemblyOccurrenceView()
                    ->occurrence.path.entries().front() == insertedId;
        undo->trigger();
        application.processEvents();
        const bool insertUndoBoundary =
            MainWindowUndoTestAccess::history(window).cursor() == 0 &&
            assemblyOccurrenceRows(*tree) == 0 &&
            MainWindowUndoTestAccess::product(window).definitions().size() ==
                1 &&
            !viewport->selectedAssemblyOccurrenceView();
        bool everyRedoBoundary = mixedRevisions.size() == 7;
        for (int step = 0; step < 7; ++step) {
          redo->trigger();
          application.processEvents();
          const auto& redoHistory = MainWindowUndoTestAccess::history(window);
          const auto* redoRoot = MainWindowUndoTestAccess::product(window)
                                     .rootAssemblyDefinition();
          const auto* redoSource =
              redoRoot ? redoRoot->find(insertedId) : nullptr;
          const auto* redoDuplicate =
              redoRoot ? redoRoot->find(duplicatedId) : nullptr;
          const bool sourceExact =
              redoSource && redoSource->targetDefinitionId() == insertedTarget &&
              redoSource->placement() == insertedPlacement &&
              redoSource->flags() == OccurrenceFlags{} &&
              redoSource->displayNameUtf8().empty();
          const bool historyExact =
              redoHistory.cursor() == static_cast<std::size_t>(step + 1) &&
              redoHistory.currentRevision() ==
                  mixedRevisions[static_cast<std::size_t>(step)] &&
              !redoHistory.isAtSavepoint() && window.isWindowModified();
          bool commandExact = false;
          if (step == 0) {
            commandExact = redoRoot && redoRoot->size() == 1 &&
                           !redoDuplicate &&
                           viewport->selectedAssemblyOccurrenceView() &&
                           viewport->selectedAssemblyOccurrenceView()
                                   ->occurrence.path.entries().front() ==
                               insertedId;
          } else if (step == 6) {
            commandExact = redoRoot && redoRoot->size() == 1 &&
                           !redoDuplicate &&
                           !viewport->selectedAssemblyOccurrenceView();
          } else {
            const bool duplicateExact =
                redoDuplicate &&
                redoDuplicate->targetDefinitionId() == insertedTarget &&
                redoDuplicate->placement() == insertedPlacement &&
                redoDuplicate->displayNameUtf8() ==
                    (step >= 2 ? "Полная цепочка 🔩" : "") &&
                redoDuplicate->flags().visible == (step < 3) &&
                redoDuplicate->flags().grounded == (step >= 4) &&
                redoDuplicate->flags().suppressed == (step >= 5);
            commandExact = redoRoot && redoRoot->size() == 2 &&
                           duplicateExact &&
                           viewport->selectedAssemblyOccurrenceView() &&
                           viewport->selectedAssemblyOccurrenceView()
                                   ->occurrence.path.entries().front() ==
                               duplicatedId;
          }
          const bool definitionUsed =
              insertedTarget.isValid() &&
              MainWindowUndoTestAccess::product(window)
                  .definitions()
                  .findPart(insertedTarget) &&
              !definitionIsOrphan(*tree, insertedTarget);
          everyRedoBoundary = everyRedoBoundary && sourceExact && historyExact &&
                              commandExact && definitionUsed;
        }
        const bool fullRedoBoundary =
            MainWindowUndoTestAccess::history(window).cursor() == 7 &&
            assemblyOccurrenceRows(*tree) == 1 &&
            MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->find(insertedId) &&
            !MainWindowUndoTestAccess::product(window)
                 .rootAssemblyDefinition()
                 ->find(duplicatedId) &&
            !viewport->selectedAssemblyOccurrenceView();
        mixedChain = duplicated && renamed && exactFields &&
                     removedAtBoundary && undoRemoveExact &&
                     duplicateUndoBoundary && insertUndoBoundary &&
                     everyRedoBoundary && fullRedoBoundary;
      }
      a5c6MixedChain = mixedChain;

      const auto redoneEpoch =
          redoneSession ? redoneSession->epoch() : ProductPresentationEpoch{};

      // A5c2: load a dedicated two-occurrence/one-definition product so the
      // native renderer assertion measures actual source sharing. Repeating
      // Insert here would author a second PartDefinition and make hiding it a
      // legitimate last-user prune instead.
      error.clear();
      if (window.loadProject(sharedAssemblyPath, &error) && error.isEmpty()) {
        application.processEvents();
        auto* visibilityModel =
            qobject_cast<AssemblyTreeModel*>(tree->model());
        const QModelIndex visibilityRoot =
            visibilityModel ? visibilityModel->index(0, 0) : QModelIndex{};
        const QModelIndex firstVisible =
            visibilityModel
                ? visibilityModel->index(0, 0, visibilityRoot)
                : QModelIndex{};
        const QModelIndex secondVisible =
            visibilityModel
                ? visibilityModel->index(1, 0, visibilityRoot)
                : QModelIndex{};
        const auto firstVisibleAddress =
            visibilityModel ? visibilityModel->addressForIndex(firstVisible)
                            : AssemblyTreeModel::AddressView{};
        const auto secondVisibleAddress =
            visibilityModel ? visibilityModel->addressForIndex(secondVisible)
                            : AssemblyTreeModel::AddressView{};
        const auto* sharedFixtureSession = viewport->assemblySession();
        const bool exactSharedFixture =
            firstVisibleAddress && secondVisibleAddress &&
            firstVisibleAddress->get().kind ==
                AssemblyTreeNodeKind::Occurrence &&
            secondVisibleAddress->get().kind ==
                AssemblyTreeNodeKind::Occurrence &&
            firstVisibleAddress->get().definitionId ==
                secondVisibleAddress->get().definitionId &&
            sharedFixtureSession && sharedFixtureSession->scene() &&
            sharedFixtureSession->scene()->sources().size() == 1 &&
            sharedFixtureSession->scene()->instances().size() == 2 &&
            sharedFixtureSession->scene()->instances()[0].sourceKey ==
                sharedFixtureSession->scene()->instances()[1].sourceKey;
        const auto currentOccurrenceIndex = [tree](int row) {
          auto* currentModel =
              qobject_cast<AssemblyTreeModel*>(tree->model());
          if (!currentModel) return QModelIndex{};
          const QModelIndex root = currentModel->index(0, 0);
          return currentModel->index(row, 0, root);
        };
        if (secondVisible.isValid()) tree->setCurrentIndex(secondVisible);
        application.processEvents();
        a5c2Entrypoints = visibility->isEnabled() &&
                          visibilityButton->isEnabled() &&
                          visibilityShortcut->isEnabled() &&
                          visibilityShortcut->key() ==
                              QKeySequence(Qt::Key_Space) &&
                          isolation->isEnabled() &&
                          isolationButton->isEnabled() &&
                          treeContextMenuHasA5c2Actions(*tree);

        const auto sharedGroundingUploads =
            viewport->rendererResourceDiagnostics();
        grounding->trigger();
        application.processEvents();
        const bool sharedGrounded =
            tree->model()
                ->data(tree->currentIndex(),
                       kAssemblyTreeAuthoredGroundedRole)
                .toBool();
        a5c3NoRemeshUpload =
            a5c3NoRemeshUpload && sharedGrounded &&
            viewport->assemblySession() &&
            viewport->assemblySession()->sceneDiagnostics().meshBuildAttempts ==
                0 &&
            (headlessPlatform ||
             sameUploadState(sharedGroundingUploads,
                             viewport->rendererResourceDiagnostics()));

        const auto sharedBefore = viewport->rendererResourceDiagnostics();
        const auto frontPoint = [](const AssemblyViewportSceneInstance& item) {
          const auto center = item.worldBounds.center();
          return Point3d{center.x, center.y, item.worldBounds.maximum().z};
        };
        std::vector<SpatialProbePoint> visibleProbePoints;
        if (viewport->assemblySession() &&
            viewport->assemblySession()->scene()) {
          for (const auto& item :
               viewport->assemblySession()->scene()->instances())
            visibleProbePoints.push_back(
                {item.occurrenceIdentity, frontPoint(item)});
        }
        const auto beforeHideSpatial = viewport->assemblySession()
            ? probeSpatialConsumers(*viewport->assemblySession(),
                                    visibleProbePoints, {})
            : test::A5c2SpatialConsumerEvidence{};
        const auto sharedMesh =
            viewport->assemblySession()->scene()->sources().empty()
                ? std::shared_ptr<const BodyRenderMesh>{}
                : viewport->assemblySession()->scene()->sources().front().mesh;

        const auto* suppressionBeforeSession = viewport->assemblySession();
        const auto suppressionBeforePublication =
            suppressionBeforeSession->publicationHandle();
        const auto suppressionBeforeEpoch = suppressionBeforeSession->epoch();
        const auto suppressionBeforeSelection =
            viewport->selectedAssemblyOccurrenceView();
        suppression->trigger();
        application.processEvents();
        const auto* suppressedSession = viewport->assemblySession();
        const auto suppressedSelection =
            viewport->selectedAssemblyOccurrenceView();
        const bool suppressedRole =
            tree->model()
                ->data(tree->currentIndex(),
                       kAssemblyTreeAuthoredSuppressedRole)
                .toBool();
        const bool visibleWhileSuppressed =
            tree->model()
                ->data(tree->currentIndex(), kAssemblyTreeAuthoredVisibleRole)
                .toBool();
        const bool groundedWhileSuppressed =
            tree->model()
                ->data(tree->currentIndex(), kAssemblyTreeAuthoredGroundedRole)
                .toBool();
        const bool suppressedAbsent =
            suppressedSession && suppressedSelection &&
            std::none_of(
                suppressedSession->scene()->instances().begin(),
                suppressedSession->scene()->instances().end(),
                [&suppressedSelection](const auto& instance) {
                  return instance.occurrenceIdentity ==
                         suppressedSelection->occurrence;
                });
        std::vector<SpatialProbePoint> suppressionRemaining;
        std::vector<SpatialProbePoint> suppressionExcluded;
        for (const auto& item : visibleProbePoints) {
          if (suppressedSelection &&
              item.occurrence == suppressedSelection->occurrence)
            suppressionExcluded.push_back(item);
          else
            suppressionRemaining.push_back(item);
        }
        const auto suppressedSpatial = suppressedSession
            ? probeSpatialConsumers(*suppressedSession,
                                    suppressionRemaining,
                                    suppressionExcluded)
            : test::A5c2SpatialConsumerEvidence{};
        a5c4OnlySuppressed = suppressedRole && visibleWhileSuppressed &&
                             groundedWhileSuppressed && suppressedAbsent;
        a5c4VisibilityGroundedIndependent =
            visibleWhileSuppressed && groundedWhileSuppressed;
        a5c4SuppressedSpatialExcluded =
            suppressedAbsent &&
            test::validA5c2SpatialConsumerEvidence(beforeHideSpatial) &&
            test::validA5c2SpatialConsumerEvidence(suppressedSpatial);
        a5c4SelectionPublicationRefresh =
            suppressedSession &&
            suppressedSession != suppressionBeforeSession &&
            suppressedSession->epoch() == suppressionBeforeEpoch &&
            !suppressedSession->publicationHandle()
                 .isSameCurrentPublication(suppressionBeforePublication) &&
            suppressedSelection && suppressionBeforeSelection &&
            suppressedSelection->occurrence ==
                suppressionBeforeSelection->occurrence &&
            suppressedSelection->isCurrentFor(
                suppressedSession->publicationHandle()) &&
            !suppressedSession->selectedHighlight();
        a5c4SharedSourceNoRemeshUpload =
            exactSharedFixture && suppressedSession &&
            suppressedSession->scene()->sources().size() == 1 &&
            suppressedSession->scene()->sources().front().mesh == sharedMesh &&
            suppressedSession->sceneDiagnostics().meshBuildAttempts == 0 &&
            (headlessPlatform ||
             sameUploadState(sharedBefore,
                             viewport->rendererResourceDiagnostics()));
        undo->trigger();
        application.processEvents();
        const bool suppressionUndone =
            viewport->assemblySession()->scene()->instances().size() == 2 &&
            viewport->assemblySession()->selectedHighlight();
        redo->trigger();
        application.processEvents();
        const bool suppressionRedone =
            viewport->assemblySession()->scene()->instances().size() == 1 &&
            !viewport->assemblySession()->selectedHighlight();
        a5c4UndoRedo = suppressionUndone && suppressionRedone;
        const bool unsuppressShortcut = QMetaObject::invokeMethod(
            suppressionShortcut, "activated", Qt::DirectConnection);
        application.processEvents();
        a5c4SuppressUnsuppress =
            a5c4SuppressUnsuppress && unsuppressShortcut &&
            !tree->model()
                 ->data(tree->currentIndex(),
                        kAssemblyTreeAuthoredSuppressedRole)
                 .toBool() &&
            viewport->assemblySession()->scene()->instances().size() == 2;

        visibility->trigger();
        application.processEvents();
        const auto hiddenSelection =
            viewport->selectedAssemblyOccurrenceView();
        const auto* hiddenSession = viewport->assemblySession();
        const bool hiddenAbsent =
            hiddenSelection && hiddenSession && hiddenSession->scene() &&
            std::none_of(
                hiddenSession->scene()->instances().begin(),
                hiddenSession->scene()->instances().end(),
                [&hiddenSelection](const auto& instance) {
                  return instance.occurrenceIdentity ==
                         hiddenSelection->occurrence;
                });
        a5c2OnlyVisible = hiddenSession &&
                          hiddenSession->scene()->instances().size() == 1 &&
                          hiddenAbsent && !hiddenSession->selectedHighlight();
        const bool groundedWhileHidden =
            tree->model()
                ->data(tree->currentIndex(),
                       kAssemblyTreeAuthoredGroundedRole)
                .toBool();
        std::vector<SpatialProbePoint> remainingProbePoints;
        std::vector<SpatialProbePoint> hiddenProbePoints;
        for (const auto& item : visibleProbePoints) {
          if (hiddenSelection &&
              item.occurrence == hiddenSelection->occurrence)
            hiddenProbePoints.push_back(item);
          else
            remainingProbePoints.push_back(item);
        }
        const auto hiddenSpatial = hiddenSession
            ? probeSpatialConsumers(*hiddenSession, remainingProbePoints,
                                    hiddenProbePoints)
            : test::A5c2SpatialConsumerEvidence{};
        a5c2HiddenSpatialExcluded =
            hiddenAbsent && hiddenSession &&
            hiddenSession->picking().diagnostics().activeInstances == 1 &&
            test::validA5c2SpatialConsumerEvidence(beforeHideSpatial) &&
            test::validA5c2SpatialConsumerEvidence(hiddenSpatial);
        a5c2SharedSourceNoRemeshUpload =
            exactSharedFixture && hiddenSession &&
            hiddenSession->scene()->sources().size() == 1 &&
            hiddenSession->scene()->sources().front().mesh == sharedMesh &&
            hiddenSession->sceneDiagnostics().meshBuildAttempts == 0 &&
            (headlessPlatform ||
             sameUploadState(sharedBefore,
                             viewport->rendererResourceDiagnostics()));
        const bool hideTextChanged =
            visibility->text() == QString::fromUtf8("Показать экземпляр");
        undo->trigger();
        application.processEvents();
        const bool shownByUndo =
            viewport->assemblySession()->scene()->instances().size() == 2 &&
            viewport->assemblySession()->selectedHighlight();
        const bool groundedAfterVisibilityUndo =
            tree->model()
                ->data(tree->currentIndex(),
                       kAssemblyTreeAuthoredGroundedRole)
                .toBool();
        redo->trigger();
        application.processEvents();
        const bool hiddenByRedo =
            viewport->assemblySession()->scene()->instances().size() == 1 &&
            !viewport->assemblySession()->selectedHighlight();
        const bool groundedAfterVisibilityRedo =
            tree->model()
                ->data(tree->currentIndex(),
                       kAssemblyTreeAuthoredGroundedRole)
                .toBool();
        a5c2VisibilityUndoRedo = hideTextChanged && shownByUndo && hiddenByRedo;
        a5c3VisibilitySuppressionIndependent =
            sharedGrounded && groundedWhileHidden &&
            groundedAfterVisibilityUndo && groundedAfterVisibilityRedo &&
            a5c3PlacementGuard;
        undo->trigger();
        application.processEvents();

        // Hide both shared-source users. The final hide intentionally drops
        // the source; showing one later must rebuild/reacquire it.
        tree->setCurrentIndex(currentOccurrenceIndex(0));
        application.processEvents();
        visibility->trigger();
        application.processEvents();
        tree->setCurrentIndex(currentOccurrenceIndex(1));
        application.processEvents();
        const auto beforeLastHide = viewport->rendererResourceDiagnostics();
        visibility->trigger();
        application.processEvents();
        const bool sourcePruned =
            viewport->assemblySession()->scene()->sources().empty() &&
            viewport->assemblySession()->sceneDiagnostics().residentSources ==
                0 &&
            (headlessPlatform ||
             viewport->rendererResourceDiagnostics().sourcePruneCount >
                 beforeLastHide.sourcePruneCount);
        const auto beforeReacquire = viewport->rendererResourceDiagnostics();
        visibility->trigger();
        application.processEvents();
        const bool sourceReacquired =
            viewport->assemblySession()->scene()->sources().size() == 1 &&
            viewport->assemblySession()->sceneDiagnostics()
                    .meshBuildAttempts > 0 &&
            (headlessPlatform ||
             viewport->rendererResourceDiagnostics().sourceUploadSuccesses >
                 beforeReacquire.sourceUploadSuccesses);
        a5c2LastUserPruneReacquire = sourcePruned && sourceReacquired;
        tree->setCurrentIndex(currentOccurrenceIndex(0));
        application.processEvents();
        visibility->trigger();
        application.processEvents();

        // Repeat the last-effective-user boundary through the A5c4 command
        // itself.  Force a rendered frame so native prune/upload counters are
        // evidence of renderer work rather than queued update timing.
        tree->setCurrentIndex(currentOccurrenceIndex(0));
        application.processEvents();
        suppression->trigger();
        application.processEvents();
        tree->setCurrentIndex(currentOccurrenceIndex(1));
        application.processEvents();
        const auto beforeLastSuppression =
            viewport->rendererResourceDiagnostics();
        suppression->trigger();
        viewport->repaint();
        application.processEvents();
        const bool suppressionSourcePruned =
            viewport->assemblySession()->scene()->sources().empty() &&
            viewport->assemblySession()->sceneDiagnostics().residentSources ==
                0 &&
            (headlessPlatform ||
             viewport->rendererResourceDiagnostics().sourcePruneCount >
                 beforeLastSuppression.sourcePruneCount);
        const auto beforeSuppressionReacquire =
            viewport->rendererResourceDiagnostics();
        suppression->trigger();
        viewport->repaint();
        application.processEvents();
        const bool suppressionSourceReacquired =
            viewport->assemblySession()->scene()->sources().size() == 1 &&
            viewport->assemblySession()->sceneDiagnostics()
                    .meshBuildAttempts > 0 &&
            (headlessPlatform ||
             viewport->rendererResourceDiagnostics().sourceUploadSuccesses >
                 beforeSuppressionReacquire.sourceUploadSuccesses);
        a5c4LastUserPruneReacquire =
            suppressionSourcePruned && suppressionSourceReacquired;
        tree->setCurrentIndex(currentOccurrenceIndex(0));
        application.processEvents();
        suppression->trigger();
        application.processEvents();

        const auto* isolateSession = viewport->assemblySession();
        const auto isolatePublication = isolateSession->publicationHandle();
        const auto isolateEpoch = isolateSession->epoch();
        const auto isolateUploads = viewport->rendererResourceDiagnostics();
        const bool isolateDirty = window.isWindowModified();
        std::vector<SpatialProbePoint> isolateBasePoints;
        if (isolateSession && isolateSession->scene()) {
          for (const auto& item : isolateSession->scene()->instances())
            isolateBasePoints.push_back(
                {item.occurrenceIdentity, frontPoint(item)});
        }
        const auto isolateSelection =
            viewport->selectedAssemblyOccurrenceView();
        std::vector<SpatialProbePoint> isolateTargetPoints;
        std::vector<SpatialProbePoint> isolateExcludedPoints;
        for (const auto& item : isolateBasePoints) {
          if (isolateSelection &&
              item.occurrence == isolateSelection->occurrence)
            isolateTargetPoints.push_back(item);
          else
            isolateExcludedPoints.push_back(item);
        }
        const auto beforeIsolateSpatial = isolateSession
            ? probeSpatialConsumers(*isolateSession, isolateBasePoints, {})
            : test::A5c2SpatialConsumerEvidence{};
        isolation->trigger();
        application.processEvents();
        const bool isolated =
            isolateSession == viewport->assemblySession() &&
            isolateSession->isolationActive() &&
            isolateSession->publicationHandle().isSameCurrentPublication(
                isolatePublication) &&
            isolateSession->epoch() == isolateEpoch &&
            isolateSession->scene()->instances().size() == 1;
        const auto duringIsolateSpatial = isolated
            ? probeSpatialConsumers(*isolateSession, isolateTargetPoints,
                                    isolateExcludedPoints)
            : test::A5c2SpatialConsumerEvidence{};
        a5c2IsolatePresentationOnly =
            isolated && window.isWindowModified() == isolateDirty &&
            !visibility->isEnabled() && !rename->isEnabled() &&
            !undo->isEnabled() && !redo->isEnabled() &&
            test::validA5c2SpatialConsumerEvidence(beforeIsolateSpatial) &&
            test::validA5c2SpatialConsumerEvidence(duringIsolateSpatial);
        a5c3IsolateDisabled = isolated && !grounding->isEnabled() &&
                              !groundingButton->isEnabled() &&
                              !groundingShortcut->isEnabled();
        a5c4IsolateDisabled = isolated && !suppression->isEnabled() &&
                              !suppressionButton->isEnabled() &&
                              !suppressionShortcut->isEnabled();
        a5c5IsolateDisabled = isolated && !duplicate->isEnabled() &&
                              !duplicateButton->isEnabled() &&
                              !duplicateShortcut->isEnabled();
        a5c6IsolateDisabled = isolated && !remove->isEnabled() &&
                              !removeButton->isEnabled() &&
                              !removeShortcut->isEnabled();
        a5c2IsolateNoUploadPrune =
            isolated &&
            test::validA5c2SpatialConsumerEvidence(duringIsolateSpatial) &&
            sameUploadState(isolateUploads,
                            viewport->rendererResourceDiagnostics());
        isolation->trigger();
        application.processEvents();
        const auto afterIsolateSpatial = isolateSession
            ? probeSpatialConsumers(*isolateSession, isolateBasePoints, {})
            : test::A5c2SpatialConsumerEvidence{};
        a5c2IsolateShowAll =
            !isolateSession->isolationActive() &&
            isolateSession == viewport->assemblySession() &&
            isolateSession->publicationHandle().isSameCurrentPublication(
                isolatePublication) &&
            isolateSession->scene()->instances().size() == 2 &&
            test::validA5c2SpatialConsumerEvidence(beforeIsolateSpatial) &&
            test::validA5c2SpatialConsumerEvidence(duringIsolateSpatial) &&
            test::validA5c2SpatialConsumerEvidence(afterIsolateSpatial) &&
            sameUploadState(isolateUploads,
                            viewport->rendererResourceDiagnostics());

        isolation->trigger();
        application.processEvents();
        std::vector<std::pair<AssemblyInsertTeardownStep, bool>> isolateTrace;
        window.setAssemblyReplacementTraceObserverForTesting(
            [&isolateTrace](AssemblyInsertTeardownStep step, bool alive) {
              isolateTrace.emplace_back(step, alive);
            });
        error.clear();
        const bool isolateReplacement =
            window.loadProject(assemblyPath, &error);
        window.setAssemblyReplacementTraceObserverForTesting({});
        application.processEvents();
        const std::vector<std::pair<AssemblyInsertTeardownStep, bool>>
            expectedIsolationTrace{
                {AssemblyInsertTeardownStep::InsertCallbacksInvalidated,
                 false},
                {AssemblyInsertTeardownStep::InsertControllerCancelled,
                 false},
                {AssemblyInsertTeardownStep::InsertSessionCancelled, false},
                {AssemblyInsertTeardownStep::PartDesignPrepared, true},
                {AssemblyInsertTeardownStep::PartDesignSessionsCancelled,
                 true},
                {AssemblyInsertTeardownStep::GhostUiDetached, false},
                {AssemblyInsertTeardownStep::AssemblyHistoryCleared, false},
                {AssemblyInsertTeardownStep::OldWorkspaceDetached, false},
                {AssemblyInsertTeardownStep::ProductReplaced, false},
                {AssemblyInsertTeardownStep::WorkspaceInstalled, false}};
        a5c2ReplacementTeardown =
            isolateReplacement && error.isEmpty() &&
            isolateTrace == expectedIsolationTrace &&
            viewport->assemblySession() &&
            !viewport->assemblySession()->isolationActive() &&
            viewport->assemblySession() != isolateSession;
        a5c3ReplacementTeardown = a5c2ReplacementTeardown;
        a5c4ReplacementTeardown = a5c2ReplacementTeardown;
        a5c5ReplacementTeardown = a5c2ReplacementTeardown;
        a5c6ReplacementTeardown = a5c2ReplacementTeardown;
      }

      // A5c6: use the bounded two-occurrence/one-definition fixture. Remove
      // must affect only one occurrence node, keep the shared definition and
      // source alive, and preserve the exact command/publication boundary.
      error.clear();
      if (window.loadProject(removeAssemblyPath, &error) && error.isEmpty()) {
        application.processEvents();
        auto occurrenceIndex = [&](int row) {
          auto* model = qobject_cast<AssemblyTreeModel*>(tree->model());
          const QModelIndex rootIndex =
              model ? model->index(0, 0) : QModelIndex{};
          return model ? model->index(row, 0, rootIndex) : QModelIndex{};
        };
        tree->setCurrentIndex(occurrenceIndex(0));
        application.processEvents();
        const auto initialSelection =
            viewport->selectedAssemblyOccurrenceView();
        const auto* initialRoot =
            MainWindowUndoTestAccess::product(window).rootAssemblyDefinition();
        const auto initialId = initialSelection
            ? initialSelection->occurrence.path.entries().front()
            : OccurrenceId{};
        const auto* initialOccurrence =
            initialRoot ? initialRoot->find(initialId) : nullptr;
        const auto target = initialOccurrence
            ? initialOccurrence->targetDefinitionId()
            : DefinitionId{};
        const auto* part =
            MainWindowUndoTestAccess::product(window).definitions().findPart(
                target);
        const auto* document = part ? &part->document() : nullptr;
        const TopoDS_Shape* brep =
            document && !document->bodies().empty()
                ? document->bodies().front().resultShape().get()
                : nullptr;
        const auto definitionIds =
            MainWindowUndoTestAccess::product(window)
                .definitions()
                .definitionIds();
        a5c6Entrypoints =
            remove->isEnabled() && removeButton->isEnabled() &&
            removeShortcut->isEnabled() &&
            removeShortcut->key() == QKeySequence(Qt::Key_Delete) &&
            removeShortcut->parent() == tree &&
            treeContextMenuHasRemoveAction(*tree);

        const auto* failureSession = viewport->assemblySession();
        const auto failurePublication = failureSession->publicationHandle();
        const auto failureResources = viewport->rendererResourceDiagnostics();
        const auto failureHistorySize =
            MainWindowUndoTestAccess::history(window).size();
        const auto failureCursor =
            MainWindowUndoTestAccess::history(window).cursor();
        const auto failureBytes =
            MainWindowUndoTestAccess::history(window).retainedBytes();
        const auto failureRevision =
            MainWindowUndoTestAccess::history(window).currentRevision();
        MainWindowUndoTestAccess::injectProjection(
            window, AssemblyViewportSessionFailurePoint::Presentation);
        remove->trigger();
        application.processEvents();
        MainWindowUndoTestAccess::injectProjection(
            window, AssemblyViewportSessionFailurePoint::None);
        a5c6FailureAtomicity =
            initialSelection == viewport->selectedAssemblyOccurrenceView() &&
            assemblyOccurrenceRows(*tree) == 2 &&
            MainWindowUndoTestAccess::history(window).size() ==
                failureHistorySize &&
            MainWindowUndoTestAccess::history(window).cursor() ==
                failureCursor &&
            MainWindowUndoTestAccess::history(window).retainedBytes() ==
                failureBytes &&
            MainWindowUndoTestAccess::history(window).currentRevision() ==
                failureRevision &&
            viewport->assemblySession() == failureSession &&
            viewport->assemblySession()->publicationHandle()
                .isSameCurrentPublication(failurePublication) &&
            sameUploadState(failureResources,
                            viewport->rendererResourceDiagnostics());

        viewport->repaint();
        application.processEvents();
        const auto* beforeRemoveSession = viewport->assemblySession();
        const auto beforeRemovePublication =
            beforeRemoveSession->publicationHandle();
        const auto beforeRemoveEpoch = beforeRemoveSession->epoch();
        const auto beforeRemoveResources =
            viewport->rendererResourceDiagnostics();
        const auto beforeRemoveInstances =
            beforeRemoveSession->sceneDiagnostics().activeInstances;
        const auto beforeRemovePicking =
            beforeRemoveSession->picking().diagnostics().activeInstances;
        remove->trigger();
        application.processEvents();
        viewport->repaint();
        application.processEvents();
        const auto* removedSession = viewport->assemblySession();
        const auto afterRemoveResources =
            viewport->rendererResourceDiagnostics();
        const auto* retainedPart =
            MainWindowUndoTestAccess::product(window).definitions().findPart(
                target);
        a5c6OnlyOccurrenceNode =
            assemblyOccurrenceRows(*tree) == 1 && retainedPart == part &&
            retainedPart && &retainedPart->document() == document &&
            (!document || document->bodies().empty() ||
             document->bodies().front().resultShape().get() == brep);
        a5c6OrphanDefinitionRetained =
            MainWindowUndoTestAccess::product(window)
                    .definitions()
                    .definitionIds() == definitionIds &&
            retainedPart && !definitionIsOrphan(*tree, target);
        a5c6SelectionClearRestore =
            !viewport->selectedAssemblyOccurrenceView() &&
            !removedSession->selectedHighlight();
        a5c6SameEpochPublicationRefresh =
            removedSession != beforeRemoveSession &&
            removedSession->epoch() == beforeRemoveEpoch &&
            !removedSession->publicationHandle().isSameCurrentPublication(
                beforeRemovePublication);
        a5c6SharedSourceNoRemeshUpload =
            removedSession->sceneDiagnostics().activeInstances ==
                beforeRemoveInstances - 1 &&
            removedSession->picking().diagnostics().activeInstances ==
                beforeRemovePicking - 1 &&
            removedSession->sceneDiagnostics().meshBuildAttempts == 0 &&
            removedSession->picking().diagnostics()
                    .topologySourceBuildAttempts == 0 &&
            (headlessPlatform ||
             sameUploadState(beforeRemoveResources, afterRemoveResources));
        const bool nativeDrawDecrement =
            headlessPlatform ||
            (afterRemoveResources.submittedAssemblyInstances + 1 ==
                 beforeRemoveResources.submittedAssemblyInstances &&
             afterRemoveResources.drawnAssemblyInstances + 1 ==
                 beforeRemoveResources.drawnAssemblyInstances);

        undo->trigger();
        application.processEvents();
        const auto restoredSelection =
            viewport->selectedAssemblyOccurrenceView();
        const auto* restoredSession = viewport->assemblySession();
        const QSize removeQuerySize{std::max(1, viewport->width()),
                                    std::max(1, viewport->height())};
        const auto restoredFit = restoredSession
            ? fitAssemblyViewport(restoredSession->scene(), removeQuerySize,
                                  0.0F, 0.0F)
            : AssemblyViewportFitResult{};
        const auto restoredRectangle = restoredSession && restoredFit
            ? restoredSession->picking().facesInRectangle(
                  {0.0F, 0.0F, restoredFit.fit->zoom,
                   restoredFit.fit->pan, removeQuerySize, 1.0F,
                   restoredFit.fit->center, restoredFit.fit->depthExtent},
                  QPointF{0.0, 0.0},
                  QPointF{static_cast<double>(removeQuerySize.width() - 1),
                          static_cast<double>(removeQuerySize.height() - 1)})
            : AssemblyFaceRectangleQueryResult{};
        const bool restoredRectangleCurrent =
            restoredSession && restoredRectangle &&
            std::all_of(restoredRectangle.hits.begin(),
                        restoredRectangle.hits.end(), [&](const auto& hit) {
                          return hit.publication.isSameCurrentPublication(
                              restoredSession->publicationHandle());
                        });
        const bool rectangleHasBoth =
            restoredRectangleCurrent && restoredRectangle.hits.size() >= 2 &&
            std::any_of(restoredRectangle.hits.begin(),
                        restoredRectangle.hits.end(), [&](const auto& hit) {
                          return hit.address.occurrence.path.entries().front() ==
                                 OccurrenceId{11};
                        }) &&
            std::any_of(restoredRectangle.hits.begin(),
                        restoredRectangle.hits.end(), [&](const auto& hit) {
                          return hit.address.occurrence.path.entries().front() ==
                                 OccurrenceId{12};
                        });
        const bool undoRestored =
            assemblyOccurrenceRows(*tree) == 2 && restoredSelection &&
            restoredSelection->occurrence.path.entries().front() == initialId;
        redo->trigger();
        application.processEvents();
        const bool redoRemoved = assemblyOccurrenceRows(*tree) == 1 &&
                                 !viewport->selectedAssemblyOccurrenceView();
        undo->trigger();
        application.processEvents();
        a5c6RemoveUndoRedo = undoRestored && redoRemoved &&
                             assemblyOccurrenceRows(*tree) == 2;
        a5c6SelectionClearRestore =
            a5c6SelectionClearRestore && undoRestored && redoRemoved;
        a5c6InstanceDrawPicking =
            nativeDrawDecrement && rectangleHasBoth;

        // Field commands followed by Remove retain every authored bit in the
        // cold payload; Undo restores the exact hidden/suppressed/grounded
        // occurrence without revealing it in the viewport.
        grounding->trigger();
        visibility->trigger();
        suppression->trigger();
        application.processEvents();
        const auto chainFlags =
            MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->find(initialId)
                ->flags();
        remove->trigger();
        application.processEvents();
        undo->trigger();
        application.processEvents();
        const auto* chainRestored =
            MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->find(initialId);
        a5c6HiddenSuppressedGrounded =
            chainRestored && !chainFlags.visible && chainFlags.suppressed &&
            chainFlags.grounded && chainRestored->flags() == chainFlags &&
            !viewport->assemblySession()->selectedHighlight();
        suppression->trigger();
        visibility->trigger();
        grounding->trigger();
        application.processEvents();

        // A colliding out-of-band reuse of the removed stable ID must leave
        // both the history cursor and current publication untouched.
        remove->trigger();
        application.processEvents();
        auto removedPath = OccurrencePath::create({initialId});
        const auto* collisionSession = viewport->assemblySession();
        const auto collisionPublication =
            collisionSession->publicationHandle();
        const auto collisionCursor =
            MainWindowUndoTestAccess::history(window).cursor();
        const bool collisionInserted =
            removedPath &&
            MainWindowUndoTestAccess::mutableProduct(window)
                    .insertRootOccurrence(ComponentOccurrence(
                        initialId, target, RigidTransform3d::identity())) ==
                ProductOccurrenceMutationStatus::Committed;
        undo->trigger();
        application.processEvents();
        a5c6UndoCollisionRollback =
            collisionInserted &&
            MainWindowUndoTestAccess::history(window).cursor() ==
                collisionCursor &&
            viewport->assemblySession() == collisionSession &&
            viewport->assemblySession()->publicationHandle()
                .isSameCurrentPublication(collisionPublication) &&
            MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->find(initialId);
        if (removedPath) {
          (void)MainWindowUndoTestAccess::mutableProduct(window)
              .removeRootOccurrence(*removedPath.path);
          undo->trigger();
          application.processEvents();
        }

        // Removing both effective users leaves a valid empty assembly root,
        // retains the orphan definition/tree row, and Undo reacquires it.
        remove->trigger();
        application.processEvents();
        tree->setCurrentIndex(occurrenceIndex(0));
        application.processEvents();
        const auto beforeEmptyCamera =
            MainWindowUndoTestAccess::assemblyCamera(window);
        const auto beforeLastPicking =
            viewport->assemblySession()->picking().diagnostics();
        const auto beforeLastRemoveResources =
            viewport->rendererResourceDiagnostics();
        remove->trigger();
        viewport->repaint();
        application.processEvents();
        const bool emptyRoot =
            assemblyOccurrenceRows(*tree) == 0 &&
            MainWindowUndoTestAccess::product(window)
                .rootAssemblyDefinition()
                ->occurrences()
                .empty();
        a5c6OrphanTreeFeedback = definitionIsOrphan(*tree, target);
        a5c6LastOccurrenceEmptyRoot =
            emptyRoot && a5c6OrphanTreeFeedback &&
            MainWindowUndoTestAccess::product(window).definitions().findPart(
                target) == part;
        const auto emptyCamera =
            MainWindowUndoTestAccess::assemblyCamera(window);
        const auto emptyPicking =
            viewport->assemblySession()->picking().diagnostics();
        a5c6EmptyCamera =
            emptyRoot && emptyCamera.yawDeg == beforeEmptyCamera.yawDeg &&
            emptyCamera.pitchDeg == beforeEmptyCamera.pitchDeg &&
            emptyCamera.zoom == beforeEmptyCamera.zoom &&
            emptyCamera.pan == beforeEmptyCamera.pan &&
            emptyCamera.logicalSize == beforeEmptyCamera.logicalSize &&
            samePoint(emptyCamera.center, Point3d{}) &&
            emptyCamera.depthExtent == 1.0;
        const bool removePruned =
            viewport->assemblySession()->scene()->sources().empty() &&
            emptyPicking.activeSources == 0 &&
            emptyPicking.activeInstances == 0 &&
            emptyPicking.residentTopologySources == 0 &&
            beforeLastPicking.residentTopologySources > 0 &&
            (headlessPlatform ||
             viewport->rendererResourceDiagnostics().sourcePruneCount >
                 beforeLastRemoveResources.sourcePruneCount);
        const auto beforeRemoveUndoResources =
            viewport->rendererResourceDiagnostics();
        undo->trigger();
        viewport->repaint();
        application.processEvents();
        const bool removeReacquired =
            viewport->assemblySession()->scene()->sources().size() == 1 &&
            viewport->assemblySession()->sceneDiagnostics().meshBuildAttempts >
                0 &&
            (headlessPlatform ||
             viewport->rendererResourceDiagnostics().sourceUploadSuccesses >
                 beforeRemoveUndoResources.sourceUploadSuccesses);
        a5c6LastUserPruneReacquire = removePruned && removeReacquired;
        const auto* reacquiredSession = viewport->assemblySession();
        const QSize reacquiredSize{std::max(1, viewport->width()),
                                   std::max(1, viewport->height())};
        const auto reacquiredFit = reacquiredSession
            ? fitAssemblyViewport(reacquiredSession->scene(), reacquiredSize,
                                  0.0F, 0.0F)
            : AssemblyViewportFitResult{};
        const auto reacquiredHits = reacquiredSession && reacquiredFit
            ? reacquiredSession->picking().facesInRectangle(
                  {0.0F, 0.0F, reacquiredFit.fit->zoom,
                   reacquiredFit.fit->pan, reacquiredSize, 1.0F,
                   reacquiredFit.fit->center, reacquiredFit.fit->depthExtent},
                  QPointF{0.0, 0.0},
                  QPointF{static_cast<double>(reacquiredSize.width() - 1),
                          static_cast<double>(reacquiredSize.height() - 1)})
            : AssemblyFaceRectangleQueryResult{};
        const auto reacquiredPicking = reacquiredSession
            ? reacquiredSession->picking().diagnostics()
            : AssemblyViewportPickingDiagnostics{};
        const bool reacquiredCurrentHit =
            reacquiredSession && reacquiredHits &&
            !reacquiredHits.hits.empty() &&
            std::all_of(reacquiredHits.hits.begin(), reacquiredHits.hits.end(),
                        [&](const auto& hit) {
                          return hit.publication.isSameCurrentPublication(
                              reacquiredSession->publicationHandle());
                        });
        a5c6TopologyPruneReacquire =
            removePruned && removeReacquired &&
            reacquiredPicking.residentTopologySources == 1 &&
            reacquiredPicking.topologySourceBuildAttempts > 0 &&
            reacquiredCurrentHit;
      }

      if (chooseSourceAndClick(*insert, partPath)) {
        const bool hadLiveGhost =
            viewport->assemblyPlacementPreview().active();
        std::vector<std::pair<AssemblyInsertTeardownStep, bool>> trace;
        window.setAssemblyReplacementTraceObserverForTesting(
            [&trace](AssemblyInsertTeardownStep step, bool alive) {
              trace.emplace_back(step, alive);
            });
        error.clear();
        const bool replacement = window.loadProject(assemblyPath, &error);
        window.setAssemblyReplacementTraceObserverForTesting({});
        application.processEvents();
        const std::vector<std::pair<AssemblyInsertTeardownStep, bool>>
            expectedTrace{
                {AssemblyInsertTeardownStep::InsertCallbacksInvalidated,
                 false},
                {AssemblyInsertTeardownStep::InsertControllerCancelled, true},
                {AssemblyInsertTeardownStep::InsertSessionCancelled, true},
                {AssemblyInsertTeardownStep::PartDesignPrepared, true},
                {AssemblyInsertTeardownStep::PartDesignSessionsCancelled,
                 true},
                {AssemblyInsertTeardownStep::GhostUiDetached, false},
                {AssemblyInsertTeardownStep::AssemblyHistoryCleared, false},
                {AssemblyInsertTeardownStep::OldWorkspaceDetached, false},
                {AssemblyInsertTeardownStep::ProductReplaced, false},
                {AssemblyInsertTeardownStep::WorkspaceInstalled, false}};
        replacementTeardownOrder =
            hadLiveGhost && replacement && error.isEmpty() &&
            trace == expectedTrace &&
            !viewport->assemblyPlacementPreview().active() &&
            viewport->assemblySession() &&
            viewport->assemblySession()->epoch() != redoneEpoch;
        a5c1ReplacementTeardown = replacementTeardownOrder;
      }
    }
  }

  // A5d keeps the embedded Document owned by ProductDocument while exposing
  // Part Design UI.  Exercise the real definition-row entry and the same
  // assembly history/projection boundary used by the editor.
  if (editDefinition && editDefinitionButton && definitionBreadcrumb &&
      definitionBack && undo && redo &&
      window.loadProject(definitionEditAssemblyPath, &error)) {
    application.processEvents();
    model = qobject_cast<AssemblyTreeModel*>(tree->model());
    constexpr DefinitionId kEditedDefinition{1};
    const QModelIndex definitionIndex =
        model ? model->indexForDefinition(kEditedDefinition) : QModelIndex{};
    const auto* rootBefore =
        MainWindowUndoTestAccess::product(window).rootAssemblyDefinition();
    const auto* firstBefore =
        rootBefore ? rootBefore->find(OccurrenceId{11}) : nullptr;
    const auto* secondBefore =
        rootBefore ? rootBefore->find(OccurrenceId{12}) : nullptr;
    const auto* unaffectedBefore =
        rootBefore ? rootBefore->find(OccurrenceId{14}) : nullptr;
    const auto firstPlacement =
        firstBefore ? firstBefore->placement() : RigidTransform3d::identity();
    const auto secondPlacement =
        secondBefore ? secondBefore->placement() : RigidTransform3d::identity();
    const auto firstFlags = firstBefore ? firstBefore->flags() : OccurrenceFlags{};
    const auto secondFlags =
        secondBefore ? secondBefore->flags() : OccurrenceFlags{};
    const auto unaffectedPlacement = unaffectedBefore
                                         ? unaffectedBefore->placement()
                                         : RigidTransform3d::identity();
    const auto unaffectedFlags = unaffectedBefore
                                     ? unaffectedBefore->flags()
                                     : OccurrenceFlags{};
    const auto unaffectedName = unaffectedBefore
                                    ? unaffectedBefore->displayNameUtf8()
                                    : std::string{};
    const auto* const documentAddress =
        MainWindowUndoTestAccess::partDocument(window, kEditedDefinition);
    const auto stateBefore =
        MainWindowUndoTestAccess::definitionState(window, kEditedDefinition);
    const auto historyBefore = MainWindowUndoTestAccess::history(window).size();
    viewport->repaint();
    application.processEvents();
    const auto rendererBefore = viewport->rendererResourceDiagnostics();
    const auto navigationBefore = ViewportNavigationState{
        29.0F, -18.0F, 1.6F, QPointF(21.0, -9.0)};
    viewport->restoreNavigationState(navigationBefore);
    const auto epochBefore = viewport->assemblySession()
                                 ? viewport->assemblySession()->epoch()
                                 : ProductPresentationEpoch{};
    const auto sourceFor = [](const AssemblyViewportSession* session,
                              DefinitionId definition) {
      if (!session || !session->scene())
        return static_cast<const AssemblyViewportSceneSource*>(nullptr);
      const auto found = std::find_if(
          session->scene()->sources().begin(), session->scene()->sources().end(),
          [definition](const auto& source) {
            return source.key.definitionId == definition.value();
          });
      return found == session->scene()->sources().end() ? nullptr : &*found;
    };
    const auto instanceFor = [](const AssemblyViewportSession* session,
                                OccurrenceId occurrence) {
      if (!session || !session->scene() || !session->scene()->snapshot())
        return static_cast<const AssemblyPresentationInstance*>(nullptr);
      const auto found = std::find_if(
          session->scene()->snapshot()->instances().begin(),
          session->scene()->snapshot()->instances().end(),
          [occurrence](const auto& instance) {
            return instance.occurrencePath().entries() ==
                   std::vector<OccurrenceId>{occurrence};
          });
      return found == session->scene()->snapshot()->instances().end()
                 ? nullptr
                 : &*found;
    };
    const auto* initialSession = viewport->assemblySession();
    const auto* editedSourceBefore = sourceFor(initialSession, DefinitionId{1});
    const auto* unaffectedSourceBefore =
        sourceFor(initialSession, DefinitionId{3});
    const auto* firstInstanceBefore =
        instanceFor(initialSession, OccurrenceId{11});
    const auto* secondInstanceBefore =
        instanceFor(initialSession, OccurrenceId{12});
    const auto* hiddenInstanceBefore =
        instanceFor(initialSession, OccurrenceId{13});
    const auto* unaffectedInstanceBefore =
        instanceFor(initialSession, OccurrenceId{14});
    const auto editedMeshBefore =
        editedSourceBefore ? editedSourceBefore->mesh : nullptr;
    const auto editedSourceKeyBefore = editedSourceBefore
                                           ? std::optional<BodyMeshKey>(
                                                 editedSourceBefore->key)
                                           : std::nullopt;
    const auto editedShapeBefore =
        editedSourceBefore ? editedSourceBefore->shape : nullptr;
    const auto unaffectedMeshBefore =
        unaffectedSourceBefore ? unaffectedSourceBefore->mesh : nullptr;
    const auto unaffectedSourceKeyBefore = unaffectedSourceBefore
                                               ? std::optional<BodyMeshKey>(
                                                     unaffectedSourceBefore->key)
                                               : std::nullopt;
    const auto unaffectedTopologyBefore =
        unaffectedSourceBefore ? unaffectedSourceBefore->topologyLineage
                               : nullptr;
    const auto firstIdentityBefore = firstInstanceBefore
                                         ? std::optional<AssemblyOccurrenceIdentity>(
                                               firstInstanceBefore->occurrenceIdentity())
                                         : std::nullopt;
    const auto secondIdentityBefore = secondInstanceBefore
                                          ? std::optional<AssemblyOccurrenceIdentity>(
                                                secondInstanceBefore->occurrenceIdentity())
                                          : std::nullopt;
    const auto firstWorldBefore = firstInstanceBefore
                                      ? firstInstanceBefore->worldTransform()
                                      : RigidTransform3d::identity();
    const auto secondWorldBefore = secondInstanceBefore
                                       ? secondInstanceBefore->worldTransform()
                                       : RigidTransform3d::identity();
    const auto unaffectedIdentityBefore = unaffectedInstanceBefore
        ? std::optional<AssemblyOccurrenceIdentity>(
              unaffectedInstanceBefore->occurrenceIdentity())
        : std::nullopt;
    const auto unaffectedWorldBefore =
        unaffectedInstanceBefore ? unaffectedInstanceBefore->worldTransform()
                                 : RigidTransform3d::identity();
    const auto firstNameBefore =
        firstBefore ? firstBefore->displayNameUtf8() : std::string{};
    const auto secondNameBefore =
        secondBefore ? secondBefore->displayNameUtf8() : std::string{};
    const bool firstVisibleBefore =
        firstInstanceBefore && firstInstanceBefore->occurrenceVisible();
    const bool firstSuppressedBefore =
        firstInstanceBefore && firstInstanceBefore->suppressed();
    const bool firstGroundedBefore =
        firstInstanceBefore && firstInstanceBefore->grounded();
    const bool secondVisibleBefore =
        secondInstanceBefore && secondInstanceBefore->occurrenceVisible();
    const bool secondSuppressedBefore =
        secondInstanceBefore && secondInstanceBefore->suppressed();
    const bool secondGroundedBefore =
        secondInstanceBefore && secondInstanceBefore->grounded();
    const bool unaffectedVisibleBefore =
        unaffectedInstanceBefore &&
        unaffectedInstanceBefore->occurrenceVisible();
    const bool unaffectedSuppressedBefore =
        unaffectedInstanceBefore && unaffectedInstanceBefore->suppressed();
    const bool unaffectedGroundedBefore =
        unaffectedInstanceBefore && unaffectedInstanceBefore->grounded();
    const bool hadHiddenInstanceBefore = hiddenInstanceBefore != nullptr;

    if (definitionIndex.isValid()) tree->setCurrentIndex(definitionIndex);
    application.processEvents();
    const bool entryEnabled = editDefinition->isEnabled() &&
                              editDefinitionButton->isEnabled();
    editDefinitionButton->click();
    application.processEvents();
    const bool entered = entryEnabled &&
                         MainWindowUndoTestAccess::definitionEditActive(window) &&
                         !viewport->assemblySession() &&
                         MainWindowUndoTestAccess::suspendedAssemblySession(window) &&
                         definitionBreadcrumb->isVisible() &&
                         definitionBreadcrumb->text() ==
                             QString::fromUtf8("Сборка 100 › Деталь 1") &&
                         definitionBack->isVisible();
    const auto bodyId = documentAddress && !documentAddress->bodies().empty()
                            ? documentAddress->bodies().front().id()
                            : kInvalidBodyId;
    const std::size_t featuresBefore =
        documentAddress && !documentAddress->bodies().empty()
            ? documentAddress->bodies().front().features().size()
            : 0;
    bool committed = false;
    if (entered && bodyId != kInvalidBodyId && definitionMove &&
        definitionMoveX && definitionMoveY && definitionMoveZ &&
        definitionMoveApply) {
      definitionMove->click();
      application.processEvents();
      viewport->moveBodyPicked(bodyId);
      definitionMoveX->setValue(6.0);
      definitionMoveY->setValue(-2.0);
      definitionMoveZ->setValue(1.5);
      application.processEvents();
      if (definitionMoveApply->isEnabled()) definitionMoveApply->click();
      application.processEvents();
      const auto* movedDocument =
          MainWindowUndoTestAccess::partDocument(window, kEditedDefinition);
      const auto* moved = movedDocument && !movedDocument->bodies().empty() &&
                                  movedDocument->bodies().front().features().size() ==
                                      featuresBefore + 1
                              ? dynamic_cast<const MoveFeature*>(
                                    movedDocument->bodies()
                                        .front()
                                        .features()
                                        .back()
                                        .get())
                              : nullptr;
      committed = moved && std::abs(moved->offsetMm().x - 6.0) <= 1e-9 &&
                  std::abs(moved->offsetMm().y + 2.0) <= 1e-9 &&
                  std::abs(moved->offsetMm().z - 1.5) <= 1e-9;
    }
    const auto* const documentAfterCommit =
        MainWindowUndoTestAccess::partDocument(window, kEditedDefinition);
    const auto* editedRoot =
        MainWindowUndoTestAccess::product(window).rootAssemblyDefinition();
    const auto* editedFirst =
        editedRoot ? editedRoot->find(OccurrenceId{11}) : nullptr;
    const auto* editedSecond =
        editedRoot ? editedRoot->find(OccurrenceId{12}) : nullptr;
    const auto* editedUnaffected =
        editedRoot ? editedRoot->find(OccurrenceId{14}) : nullptr;
    const bool fieldsStable =
        editedFirst && editedSecond && editedUnaffected &&
        editedFirst->targetDefinitionId() == kEditedDefinition &&
        editedSecond->targetDefinitionId() == kEditedDefinition &&
        editedUnaffected->targetDefinitionId() == DefinitionId{3} &&
        editedFirst->placement() == firstPlacement &&
        editedSecond->placement() == secondPlacement &&
        editedUnaffected->placement() == unaffectedPlacement &&
        editedFirst->flags() == firstFlags &&
        editedSecond->flags() == secondFlags &&
        editedUnaffected->flags() == unaffectedFlags &&
        editedFirst->displayNameUtf8() == firstNameBefore &&
        editedSecond->displayNameUtf8() == secondNameBefore &&
        editedUnaffected->displayNameUtf8() == unaffectedName;
    a5dOneOwnerOneHistory =
        committed && documentAfterCommit == documentAddress &&
        MainWindowUndoTestAccess::modelHistorySize(window) == 0 &&
        MainWindowUndoTestAccess::history(window).size() == historyBefore + 1 &&
        MainWindowUndoTestAccess::definitionState(window, kEditedDefinition) !=
            stateBefore;
    a5dIdsPlacementsFlagsStable = committed && fieldsStable;
    const Document exactBeforeRejectedMove = documentAddress
                                                 ? *documentAddress
                                                 : Document{};
    const auto stateBeforeRejectedMove =
        MainWindowUndoTestAccess::definitionState(window, kEditedDefinition);
    const auto cursorBeforeRejectedMove =
        MainWindowUndoTestAccess::history(window).cursor();
    const auto revisionBeforeRejectedMove =
        MainWindowUndoTestAccess::history(window).currentRevision();
    MainWindowUndoTestAccess::injectDefinitionMoveRecomputeFailure(window,
                                                                  true);
    bool recomputeRejectedThroughUi = false;
    if (committed && definitionMove && definitionMoveApply) {
      definitionMove->click();
      application.processEvents();
      viewport->moveBodyPicked(bodyId);
      definitionMoveX->setValue(8.0);
      application.processEvents();
      if (definitionMoveApply->isEnabled()) definitionMoveApply->click();
      application.processEvents();
      recomputeRejectedThroughUi =
          documentAddress &&
          DocumentDelta::create(exactBeforeRejectedMove, *documentAddress)
              .empty();
    }
    MainWindowUndoTestAccess::injectDefinitionMoveRecomputeFailure(window,
                                                                  false);
    a5dFailedRecomputeRollback =
        recomputeRejectedThroughUi &&
        MainWindowUndoTestAccess::partDocument(window, kEditedDefinition) ==
            documentAddress &&
        MainWindowUndoTestAccess::definitionState(window,
                                                  kEditedDefinition) ==
            stateBeforeRejectedMove &&
        MainWindowUndoTestAccess::history(window).cursor() ==
            cursorBeforeRejectedMove &&
        MainWindowUndoTestAccess::history(window).currentRevision() ==
            revisionBeforeRejectedMove;
    if (definitionMoveCancel) definitionMoveCancel->click();
    application.processEvents();

    const std::size_t featuresBeforeProjectionFailure =
        documentAddress && !documentAddress->bodies().empty()
            ? documentAddress->bodies().front().features().size()
            : 0;
    const auto stateBeforeProjectionFailure =
        MainWindowUndoTestAccess::definitionState(window, kEditedDefinition);
    const auto cursorBeforeProjectionFailure =
        MainWindowUndoTestAccess::history(window).cursor();
    MainWindowUndoTestAccess::injectProjection(
        window, AssemblyViewportSessionFailurePoint::FinalPreparation);
    bool projectionRejected = false;
    if (definitionMove && definitionMoveApply) {
      definitionMove->click();
      application.processEvents();
      viewport->moveBodyPicked(bodyId);
      definitionMoveX->setValue(11.0);
      application.processEvents();
      if (definitionMoveApply->isEnabled()) definitionMoveApply->click();
      application.processEvents();
      projectionRejected =
          documentAddress &&
          documentAddress->bodies().front().features().size() ==
              featuresBeforeProjectionFailure;
    }
    MainWindowUndoTestAccess::injectProjection(
        window, AssemblyViewportSessionFailurePoint::None);
    a5dFailedProjectionRollback =
        projectionRejected && documentAddress &&
        documentAddress->bodies().front().features().size() ==
            featuresBeforeProjectionFailure &&
        MainWindowUndoTestAccess::definitionState(window, kEditedDefinition) ==
            stateBeforeProjectionFailure &&
        MainWindowUndoTestAccess::history(window).cursor() ==
            cursorBeforeProjectionFailure;
    if (definitionMoveCancel) definitionMoveCancel->click();
    application.processEvents();

    MainWindowUndoTestAccess::undo(window);
    const bool undoShared =
        documentAddress && documentAddress->bodies().front().features().size() ==
                               featuresBefore &&
        MainWindowUndoTestAccess::history(window).cursor() == historyBefore;
    MainWindowUndoTestAccess::redo(window);
    const bool redoShared =
        documentAddress && documentAddress->bodies().front().features().size() ==
                               featuresBefore + 1 &&
        MainWindowUndoTestAccess::history(window).cursor() == historyBefore + 1;
    a5dUndoRedoShared = undoShared && redoShared &&
                            MainWindowUndoTestAccess::modelHistorySize(window) == 0;

    definitionBack->click();
    application.processEvents();
    viewport->repaint();
    application.processEvents();
    const auto* resumed = viewport->assemblySession();
    const auto navigationAfter = viewport->navigationState();
    const auto* editedSourceAfter = sourceFor(resumed, DefinitionId{1});
    const auto* unaffectedSourceAfter = sourceFor(resumed, DefinitionId{3});
    const auto* firstInstanceAfter = instanceFor(resumed, OccurrenceId{11});
    const auto* secondInstanceAfter = instanceFor(resumed, OccurrenceId{12});
    const auto* hiddenInstanceAfter = instanceFor(resumed, OccurrenceId{13});
    const auto* unaffectedInstanceAfter =
        instanceFor(resumed, OccurrenceId{14});
    const auto* rootAfter =
        MainWindowUndoTestAccess::product(window).rootAssemblyDefinition();
    const auto* firstAfter =
        rootAfter ? rootAfter->find(OccurrenceId{11}) : nullptr;
    const auto* secondAfter =
        rootAfter ? rootAfter->find(OccurrenceId{12}) : nullptr;
    const auto* unaffectedAfter =
        rootAfter ? rootAfter->find(OccurrenceId{14}) : nullptr;
    a5dEntryReturnBreadcrumb =
        entered && resumed && !MainWindowUndoTestAccess::definitionEditActive(window) &&
        !definitionBreadcrumb->isVisible() && !definitionBack->isVisible();
    a5dSelectionCameraRestore =
        resumed && resumed->epoch() == epochBefore &&
        navigationAfter.yawDeg == navigationBefore.yawDeg &&
        navigationAfter.pitchDeg == navigationBefore.pitchDeg &&
        navigationAfter.zoom == navigationBefore.zoom &&
        navigationAfter.pan == navigationBefore.pan &&
        tree->currentIndex() ==
            qobject_cast<AssemblyTreeModel*>(tree->model())
                ->indexForDefinition(kEditedDefinition);
    const bool editedOccurrencesExact =
        firstInstanceBefore && secondInstanceBefore && firstInstanceAfter &&
        secondInstanceAfter && editedSourceBefore && editedSourceAfter &&
        firstIdentityBefore && secondIdentityBefore &&
        firstInstanceAfter->occurrenceIdentity() == *firstIdentityBefore &&
        secondInstanceAfter->occurrenceIdentity() == *secondIdentityBefore &&
        firstInstanceAfter->worldTransform() == firstWorldBefore &&
        secondInstanceAfter->worldTransform() == secondWorldBefore &&
        firstInstanceAfter->occurrenceVisible() == firstVisibleBefore &&
        firstInstanceAfter->suppressed() == firstSuppressedBefore &&
        firstInstanceAfter->grounded() == firstGroundedBefore &&
        secondInstanceAfter->occurrenceVisible() == secondVisibleBefore &&
        secondInstanceAfter->suppressed() == secondSuppressedBefore &&
        secondInstanceAfter->grounded() == secondGroundedBefore &&
        firstAfter && secondAfter &&
        firstAfter->displayNameUtf8() == firstNameBefore &&
        secondAfter->displayNameUtf8() == secondNameBefore &&
        firstInstanceAfter->sourceKey() == secondInstanceAfter->sourceKey() &&
        firstInstanceAfter->sourceKey().definitionId == DefinitionId{1} &&
        editedSourceKeyBefore &&
        editedSourceAfter->key != *editedSourceKeyBefore &&
        editedSourceAfter->mesh != editedMeshBefore &&
        editedSourceAfter->shape.get() != editedShapeBefore.get() &&
        resumed->picking().diagnostics().topologySourceBuildAttempts == 1 &&
        resumed->picking().diagnostics().residentTopologySources == 2;
    const bool unaffectedSourceExact =
        unaffectedSourceBefore && unaffectedSourceAfter &&
        unaffectedInstanceBefore && unaffectedInstanceAfter &&
        unaffectedAfter &&
        unaffectedSourceKeyBefore &&
        unaffectedSourceAfter->key == *unaffectedSourceKeyBefore &&
        unaffectedSourceAfter->mesh == unaffectedMeshBefore &&
        unaffectedSourceAfter->topologyLineage == unaffectedTopologyBefore &&
        unaffectedIdentityBefore &&
        unaffectedInstanceAfter->occurrenceIdentity() ==
            *unaffectedIdentityBefore &&
        unaffectedInstanceAfter->worldTransform() == unaffectedWorldBefore &&
        unaffectedInstanceAfter->occurrenceVisible() ==
            unaffectedVisibleBefore &&
        unaffectedInstanceAfter->suppressed() ==
            unaffectedSuppressedBefore &&
        unaffectedInstanceAfter->grounded() == unaffectedGroundedBefore &&
        unaffectedAfter->placement() == unaffectedPlacement &&
        unaffectedAfter->flags() == unaffectedFlags &&
        unaffectedAfter->displayNameUtf8() == unaffectedName;
    a5dSharedEditAllOccurrences =
        resumed && resumed->scene() && resumed->scene()->instances().size() == 3 &&
        resumed->sceneDiagnostics().activeSources == 2 && fieldsStable &&
        editedOccurrencesExact;
    a5dIdsPlacementsFlagsStable =
        a5dIdsPlacementsFlagsStable && editedOccurrencesExact &&
        unaffectedSourceExact && hadHiddenInstanceBefore &&
        hiddenInstanceAfter;
    const auto rendererAfter = viewport->rendererResourceDiagnostics();
    a5dEditedSourceOneUpload =
        headlessPlatform
            ? (resumed && resumed->sceneDiagnostics().meshBuildAttempts == 1)
            : (rendererAfter.sourceUploadAttempts ==
                   rendererBefore.sourceUploadAttempts + 1 &&
               rendererAfter.sourceUploadSuccesses ==
                   rendererBefore.sourceUploadSuccesses + 1);
    bool oldSourceRetained = rendererBefore.sources.empty() && headlessPlatform;
    for (const auto& beforeSource : rendererBefore.sources) {
      for (const auto& afterSource : rendererAfter.sources) {
        if (beforeSource.key == afterSource.key &&
            beforeSource.uploadGeneration == afterSource.uploadGeneration &&
            beforeSource.contentFingerprint == afterSource.contentFingerprint)
          oldSourceRetained = true;
      }
    }
    a5dOtherSourcesReused = oldSourceRetained && unaffectedSourceExact;

    // Definition 2 in the canonical fixture is an orphan. It remains a valid
    // edit target, while the hidden/suppressed occurrence of definition 1 is
    // not revealed or otherwise rewritten by definition edits.
    error.clear();
    if (window.loadProject(definitionEditAssemblyPath, &error)) {
      application.processEvents();
      model = qobject_cast<AssemblyTreeModel*>(tree->model());
      const auto* canonicalRoot =
          MainWindowUndoTestAccess::product(window).rootAssemblyDefinition();
      const auto* hiddenBefore =
          canonicalRoot ? canonicalRoot->find(OccurrenceId{13}) : nullptr;
      const auto hiddenFlagsBefore =
          hiddenBefore ? hiddenBefore->flags() : OccurrenceFlags{};
      const QModelIndex orphanIndex =
          model ? model->indexForDefinition(DefinitionId{2}) : QModelIndex{};
      if (orphanIndex.isValid()) tree->setCurrentIndex(orphanIndex);
      application.processEvents();
      const bool orphanEnabled = editDefinition->isEnabled();
      editDefinition->trigger();
      application.processEvents();
      const bool orphanEntered =
          MainWindowUndoTestAccess::definitionEditActive(window) &&
          MainWindowUndoTestAccess::partDocument(window, DefinitionId{2});
      const auto* orphanDocument =
          MainWindowUndoTestAccess::partDocument(window, DefinitionId{2});
      const auto orphanBodyId =
          orphanDocument && !orphanDocument->bodies().empty()
              ? orphanDocument->bodies().front().id()
              : kInvalidBodyId;
      const std::size_t orphanFeaturesBefore =
          orphanDocument && !orphanDocument->bodies().empty()
              ? orphanDocument->bodies().front().features().size()
              : 0;
      const auto orphanBefore = orphanDocument
                                    ? std::make_unique<Document>(*orphanDocument)
                                    : std::unique_ptr<Document>{};
      const auto orphanStateBefore =
          MainWindowUndoTestAccess::definitionState(window, DefinitionId{2});
      const auto orphanHistorySizeBefore =
          MainWindowUndoTestAccess::history(window).size();
      const auto orphanHistoryCursorBefore =
          MainWindowUndoTestAccess::history(window).cursor();
      const auto occurrenceCountBefore =
          canonicalRoot ? canonicalRoot->occurrences().size() : 0;
      const auto orphanShapeBefore =
          orphanDocument && !orphanDocument->bodies().empty() &&
                  orphanDocument->bodies().front().activeFeature()
              ? orphanDocument->bodies().front()
                    .activeFeature()->lastValidShape()
              : ShapeFeature::ShapePtr{};
      bool orphanCommitted = false;
      if (orphanEntered && orphanBodyId != kInvalidBodyId && definitionMove &&
          definitionMoveApply) {
        definitionMove->click();
        application.processEvents();
        viewport->moveBodyPicked(orphanBodyId);
        definitionMoveX->setValue(3.0);
        application.processEvents();
        if (definitionMoveApply->isEnabled()) definitionMoveApply->click();
        application.processEvents();
        orphanCommitted =
            orphanDocument->bodies().front().features().size() ==
                orphanFeaturesBefore + 1 &&
            orphanDocument->bodies().front()
                    .activeFeature()->lastValidShape().get() !=
                orphanShapeBefore.get() &&
            MainWindowUndoTestAccess::definitionState(
                window, DefinitionId{2}) != orphanStateBefore &&
            MainWindowUndoTestAccess::history(window).size() ==
                orphanHistorySizeBefore + 1 &&
            MainWindowUndoTestAccess::history(window).cursor() ==
                orphanHistoryCursorBefore + 1;
      }
      MainWindowUndoTestAccess::undo(window);
      const auto* rootAfterOrphanUndo =
          MainWindowUndoTestAccess::product(window).rootAssemblyDefinition();
      const bool orphanUndoExact =
          orphanBefore && orphanDocument &&
          DocumentDelta::create(*orphanBefore, *orphanDocument).empty() &&
          rootAfterOrphanUndo &&
          rootAfterOrphanUndo->occurrences().size() == occurrenceCountBefore &&
          MainWindowUndoTestAccess::history(window).cursor() ==
              orphanHistoryCursorBefore;
      MainWindowUndoTestAccess::redo(window);
      const auto* rootAfterOrphanRedo =
          MainWindowUndoTestAccess::product(window).rootAssemblyDefinition();
      const bool orphanRedoExact =
          orphanDocument &&
          orphanDocument->bodies().front().features().size() ==
              orphanFeaturesBefore + 1 &&
          rootAfterOrphanRedo &&
          rootAfterOrphanRedo->occurrences().size() == occurrenceCountBefore &&
          MainWindowUndoTestAccess::history(window).cursor() ==
              orphanHistoryCursorBefore + 1;
      a5dOrphanDefinitionEdit = orphanEnabled && orphanCommitted &&
                                orphanUndoExact && orphanRedoExact;
      const auto* hiddenAfterRoot =
          MainWindowUndoTestAccess::product(window).rootAssemblyDefinition();
      const auto* hiddenAfter =
          hiddenAfterRoot ? hiddenAfterRoot->find(OccurrenceId{13}) : nullptr;
      a5dHiddenSuppressedStable =
          hiddenAfter && hiddenAfter->flags() == hiddenFlagsBefore &&
          !hiddenAfter->flags().visible && hiddenAfter->flags().suppressed;
      if (definitionMove && definitionMoveApply && orphanBodyId != kInvalidBodyId) {
        definitionMove->click();
        application.processEvents();
        viewport->moveBodyPicked(orphanBodyId);
        definitionMoveX->setValue(5.0);
        definitionMoveY->setValue(-1.0);
      }
      const bool liveMoveAtReplacement =
          MainWindowUndoTestAccess::activeTool(window) ==
              PartDesignToolKind::Move &&
          MainWindowUndoTestAccess::previewHasPending(window);
      const auto previewBeforeReplacement =
          MainWindowUndoTestAccess::previewCounters(window);
      std::vector<std::pair<AssemblyInsertTeardownStep, bool>> teardownTrace;
      window.setAssemblyReplacementTraceObserverForTesting(
          [&teardownTrace](AssemblyInsertTeardownStep step, bool sourceAlive) {
            teardownTrace.emplace_back(step, sourceAlive);
          });
      error.clear();
      const bool replaced = window.loadProject(assemblyPath, &error);
      window.setAssemblyReplacementTraceObserverForTesting({});
      application.processEvents();
      const auto previewAfterReplacement =
          MainWindowUndoTestAccess::previewCounters(window);
      const std::vector<std::pair<AssemblyInsertTeardownStep, bool>>
          expectedDefinitionTrace{
              {AssemblyInsertTeardownStep::InsertCallbacksInvalidated, false},
              {AssemblyInsertTeardownStep::InsertControllerCancelled, false},
              {AssemblyInsertTeardownStep::InsertSessionCancelled, false},
              {AssemblyInsertTeardownStep::PartDesignPrepared, true},
              {AssemblyInsertTeardownStep::PartDesignSessionsCancelled, true},
              {AssemblyInsertTeardownStep::GhostUiDetached, false},
              {AssemblyInsertTeardownStep::AssemblyHistoryCleared, false},
              {AssemblyInsertTeardownStep::OldWorkspaceDetached, false},
              {AssemblyInsertTeardownStep::ProductReplaced, false},
              {AssemblyInsertTeardownStep::WorkspaceInstalled, false}};
      a5dToolReplacementTeardown =
          liveMoveAtReplacement && replaced && error.isEmpty() &&
          teardownTrace == expectedDefinitionTrace &&
          previewAfterReplacement.executions ==
              previewBeforeReplacement.executions &&
          previewAfterReplacement.publications ==
              previewBeforeReplacement.publications &&
          previewAfterReplacement.invalidations >
              previewBeforeReplacement.invalidations &&
          previewAfterReplacement.cancellations >
              previewBeforeReplacement.cancellations &&
          !MainWindowUndoTestAccess::definitionEditActive(window) &&
          !MainWindowUndoTestAccess::suspendedAssemblySession(window) &&
          viewport->assemblySession();
    }
  }

  bool a6b2InitialActive = false;
  bool a6b2Reload = false;
  bool a6b2ColdUndo = false;
  bool a6b2ColdRedo = false;
  bool a6b2StableFields = false;
  bool a6b2SelectionHover = false;
  bool a6b2SharedSource = false;
  bool a6b2SourceRetirement = false;
  bool a6b2OneUploadPerRevision = false;
  {
    error.clear();
    CHECK(window.loadProject(a6b2AssemblyPath, &error));
    CHECK(error.isEmpty());
    application.processEvents();
    model = qobject_cast<AssemblyTreeModel*>(tree->model());
    CHECK(model);
    CHECK(MainWindowUndoTestAccess::linkedRuntimeStatus(
              window, kA6b2LinkedDefinition) ==
          LinkedPartRuntimeStatus::Exact);
    const QModelIndex rootIndex = model->index(0, 0);
    const QModelIndex leftIndex = model->index(0, 0, rootIndex);
    CHECK(leftIndex.isValid());
    tree->setCurrentIndex(leftIndex);
    application.processEvents();
    const auto selected = viewport->selectedAssemblyOccurrenceView();
    CHECK(selected);
    CHECK(selected->occurrence.path.entries() ==
          std::vector<OccurrenceId>{kA6b2LeftOccurrence});
    auto* initialSession = const_cast<AssemblyViewportSession*>(
        viewport->assemblySession());
    CHECK(initialSession);
    CHECK(initialSession->setHoveredOccurrence(selected));
    viewport->repaint();
    application.processEvents();

    const auto phaseIsExact = [&](const AssemblyViewportSession* session) {
      if (!session || !session->scene() || !session->scene()->isCurrent() ||
          !session->publicationHandle().isCurrent() ||
          session->scene()->sources().size() != 1 ||
          session->scene()->instances().size() != 2 ||
          session->sceneDiagnostics().activeSources != 1 ||
          session->sceneDiagnostics().activeInstances != 2 ||
          session->sceneDiagnostics().meshBuildAttempts != 1)
        return false;
      const auto& source = session->scene()->sources().front();
      const auto& first = session->scene()->instances()[0];
      const auto& second = session->scene()->instances()[1];
      const auto currentSelection = viewport->selectedAssemblyOccurrenceView();
      const auto& currentHover = session->hoveredOccurrence();
      const auto resources = viewport->rendererResourceDiagnostics();
      return source.mesh && first.mesh == source.mesh &&
             second.mesh == source.mesh && first.sourceKey == source.key &&
             second.sourceKey == source.key &&
             first.occurrenceIdentity.path.entries() !=
                 second.occurrenceIdentity.path.entries() &&
             currentSelection && currentHover &&
             currentSelection->isCurrentFor(session->publicationHandle()) &&
             currentHover->isCurrentFor(session->publicationHandle()) &&
             currentSelection->occurrence.path.entries() ==
                 std::vector<OccurrenceId>{kA6b2LeftOccurrence} &&
             currentHover->occurrence.path.entries() ==
                 std::vector<OccurrenceId>{kA6b2LeftOccurrence} &&
             session->selectedHighlight() && session->hoveredHighlight() &&
             session->selectedHighlight()->occurrence().path ==
                 currentSelection->occurrence.path &&
             session->hoveredHighlight()->occurrence().path ==
                 currentHover->occurrence.path &&
             resources.initialized && resources.sourceMeshes == 1 &&
             resources.sources.size() == 1 &&
             resources.sources.front().key == source.key &&
             resources.activeAssemblyUsers == 2 &&
             resources.submittedAssemblyInstances == 2 &&
             resources.drawnAssemblyInstances == 2 &&
             resources.activePublicationGeneration ==
                 session->publicationHandle().generation().value();
    };
    const auto treeSelectsLeft = [&] {
      const auto* currentModel =
          qobject_cast<AssemblyTreeModel*>(tree->model());
      if (!currentModel) return false;
      const auto address =
          currentModel->addressForIndex(tree->currentIndex());
      return address && address->get().occurrence &&
             address->get().occurrence->occurrence.path.entries() ==
                 std::vector<OccurrenceId>{kA6b2LeftOccurrence};
    };

    CHECK(phaseIsExact(initialSession));
    CHECK(treeSelectsLeft());
    const auto authoredFields =
        a6b2OccurrenceFields(MainWindowUndoTestAccess::product(window));
    const auto initialPublication = initialSession->publicationHandle();
    CHECK(initialPublication.generation().isValid());
    QPointer<AssemblyTreeModel> initialTree(model);
    std::weak_ptr<const AssemblyViewportScene> initialScene(
        initialSession->scene());
    std::weak_ptr<const BodyRenderMesh> initialMesh(
        initialSession->scene()->sources().front().mesh);
    const BodyMeshKey initialSourceKey =
        initialSession->scene()->sources().front().key;
    const auto* initialShape =
        initialSession->scene()->sources().front().shape.get();
    const auto initialResources = viewport->rendererResourceDiagnostics();
    a6b2InitialActive = true;

    CHECK(writeBytes(a6b2SourcePath, a6b2RevisedEncoded.payload->bytes));
    MainWindowUndoTestAccess::reloadSelectedLinkedPart(window);
    application.processEvents();
    auto* reloadedSession = const_cast<AssemblyViewportSession*>(
        viewport->assemblySession());
    CHECK(reloadedSession);
    CHECK(reloadedSession != initialSession);
    CHECK(!initialPublication.isCurrent());
    CHECK(!reloadedSession->publicationHandle().isSameCurrentPublication(
        initialPublication));
    CHECK(reloadedSession->publicationHandle().generation().isValid());
    CHECK(qobject_cast<AssemblyTreeModel*>(tree->model()) != model);
    CHECK(initialTree.isNull());
    viewport->repaint();
    application.processEvents();
    const auto reloadResources = viewport->rendererResourceDiagnostics();
    CHECK(phaseIsExact(reloadedSession));
    CHECK(treeSelectsLeft());
    CHECK(reloadedSession->scene()->sources().front().key != initialSourceKey);
    CHECK(reloadedSession->scene()->sources().front().shape.get() !=
          initialShape);
    CHECK(initialScene.expired());
    CHECK(initialMesh.expired());
    CHECK(reloadResources.sourceUploadAttempts ==
          initialResources.sourceUploadAttempts + 1);
    CHECK(reloadResources.sourceUploadSuccesses ==
          initialResources.sourceUploadSuccesses + 1);
    CHECK(reloadResources.sourcePruneCount >
          initialResources.sourcePruneCount);
    CHECK(MainWindowUndoTestAccess::history(window).cursor() == 1);
    CHECK(a6b2OccurrenceFields(MainWindowUndoTestAccess::product(window)) ==
          authoredFields);
    a6b2Reload = true;
    a6b2SharedSource = true;
    a6b2SelectionHover = true;

    const auto reloadPublication = reloadedSession->publicationHandle();
    QPointer<AssemblyTreeModel> reloadTree(
        qobject_cast<AssemblyTreeModel*>(tree->model()));
    std::weak_ptr<const AssemblyViewportScene> reloadScene(
        reloadedSession->scene());
    std::weak_ptr<const BodyRenderMesh> reloadMesh(
        reloadedSession->scene()->sources().front().mesh);
    const BodyMeshKey reloadSourceKey =
        reloadedSession->scene()->sources().front().key;
    const auto* reloadShape =
        reloadedSession->scene()->sources().front().shape.get();
    CHECK(QFile::remove(a6b2SourcePath));

    MainWindowUndoTestAccess::undo(window);
    application.processEvents();
    auto* undoSession = const_cast<AssemblyViewportSession*>(
        viewport->assemblySession());
    CHECK(undoSession);
    CHECK(undoSession != reloadedSession);
    CHECK(!reloadPublication.isCurrent());
    CHECK(!undoSession->publicationHandle().isSameCurrentPublication(
        reloadPublication));
    CHECK(reloadTree.isNull());
    viewport->repaint();
    application.processEvents();
    const auto undoResources = viewport->rendererResourceDiagnostics();
    CHECK(phaseIsExact(undoSession));
    CHECK(treeSelectsLeft());
    CHECK(!QFile::exists(a6b2SourcePath));
    CHECK(undoSession->scene()->sources().front().key != reloadSourceKey);
    CHECK(undoSession->scene()->sources().front().shape.get() != reloadShape);
    CHECK(reloadScene.expired());
    CHECK(reloadMesh.expired());
    CHECK(undoResources.sourceUploadAttempts ==
          reloadResources.sourceUploadAttempts + 1);
    CHECK(undoResources.sourceUploadSuccesses ==
          reloadResources.sourceUploadSuccesses + 1);
    CHECK(undoResources.sourcePruneCount > reloadResources.sourcePruneCount);
    CHECK(MainWindowUndoTestAccess::history(window).cursor() == 0);
    CHECK(MainWindowUndoTestAccess::linkedRuntimeStatus(
              window, kA6b2LinkedDefinition) ==
          LinkedPartRuntimeStatus::Unchecked);
    CHECK(a6b2OccurrenceFields(MainWindowUndoTestAccess::product(window)) ==
          authoredFields);
    a6b2ColdUndo = true;

    const auto undoPublication = undoSession->publicationHandle();
    QPointer<AssemblyTreeModel> undoTree(
        qobject_cast<AssemblyTreeModel*>(tree->model()));
    std::weak_ptr<const AssemblyViewportScene> undoScene(undoSession->scene());
    std::weak_ptr<const BodyRenderMesh> undoMesh(
        undoSession->scene()->sources().front().mesh);
    const BodyMeshKey undoSourceKey =
        undoSession->scene()->sources().front().key;
    const auto* undoShape = undoSession->scene()->sources().front().shape.get();

    MainWindowUndoTestAccess::redo(window);
    application.processEvents();
    auto* redoSession = const_cast<AssemblyViewportSession*>(
        viewport->assemblySession());
    CHECK(redoSession);
    CHECK(redoSession != undoSession);
    CHECK(!undoPublication.isCurrent());
    CHECK(!redoSession->publicationHandle().isSameCurrentPublication(
        undoPublication));
    CHECK(undoTree.isNull());
    viewport->repaint();
    application.processEvents();
    const auto redoResources = viewport->rendererResourceDiagnostics();
    CHECK(phaseIsExact(redoSession));
    CHECK(treeSelectsLeft());
    CHECK(!QFile::exists(a6b2SourcePath));
    CHECK(redoSession->scene()->sources().front().key != undoSourceKey);
    CHECK(redoSession->scene()->sources().front().shape.get() != undoShape);
    CHECK(undoScene.expired());
    CHECK(undoMesh.expired());
    CHECK(redoResources.sourceUploadAttempts ==
          undoResources.sourceUploadAttempts + 1);
    CHECK(redoResources.sourceUploadSuccesses ==
          undoResources.sourceUploadSuccesses + 1);
    CHECK(redoResources.sourcePruneCount > undoResources.sourcePruneCount);
    CHECK(MainWindowUndoTestAccess::history(window).cursor() == 1);
    CHECK(MainWindowUndoTestAccess::linkedRuntimeStatus(
              window, kA6b2LinkedDefinition) ==
          LinkedPartRuntimeStatus::Unchecked);
    CHECK(a6b2OccurrenceFields(MainWindowUndoTestAccess::product(window)) ==
          authoredFields);
    a6b2ColdRedo = true;
    a6b2StableFields = true;
    a6b2SourceRetirement = true;
    a6b2OneUploadPerRevision = true;
  }

  auto a6c1Evidence = runA6c1V8GpuScenario(
      application, settings, a6c1AssemblyPath, a6c1SourcePath,
      a6c1RevisedEncoded.payload->bytes);

  AssemblyInsertToolSession directSession;
  AssemblyInsertController directController;
  directController.activate(directSession, DefinitionId{101},
                            OccurrenceId{202});
  auto directSource =
      ProjectApplicationService{}.stageEmbeddedPart(partPath);
  const bool directAccepted =
      directSource.succeeded() &&
      directSession.acceptSource(std::move(*directSource.staged));
  const auto directCheckpoint = directSession.placement();
  const bool enteredNested = directSession.beginNestedTransform();
  static_cast<void>(directSession.updateTranslation({3.0, 4.0, 5.0}));
  const bool firstEscape = directController.handleEscape();
  const bool restoredCheckpoint = directSession.placement() == directCheckpoint;
  const bool secondEscape = !directController.handleEscape();
  const bool escapeNestedThenTool =
      directAccepted && enteredNested && firstEscape && restoredCheckpoint &&
      secondEscape &&
      directSession.state() == AssemblyInsertToolState::Inactive;

  test::A8aGpuSmokeEvidence a8aEvidence;
  QString a8aError;
  if (window.loadProject(sharedAssemblyPath, &a8aError)) {
    application.processEvents();
    auto hits = MainWindowUndoTestAccess::mateHits(window);
    if (hits.size() >= 2) {
      MainWindowUndoTestAccess::beginMate(window);
      MainWindowUndoTestAccess::acceptMate(window, hits[0]);
      a8aEvidence.typedTwoStage =
          MainWindowUndoTestAccess::mateState(window) ==
          AssemblyMateToolState::SelectingSecond;
      a8aEvidence.occurrenceScope =
          hits[0].address.occurrence.path != hits[1].address.occurrence.path;
      auto invalid = hits[1];
      invalid.address.face = FaceReference{};
      const auto stateBeforeInvalid =
          MainWindowUndoTestAccess::mateState(window);
      MainWindowUndoTestAccess::acceptMate(window, invalid);
      a8aEvidence.strictFilter =
          MainWindowUndoTestAccess::mateState(window) == stateBeforeInvalid;
      MainWindowUndoTestAccess::acceptMate(window, hits[1]);
      const auto* alignedScene =
          MainWindowUndoTestAccess::matePreviewScene(window);
      MainWindowUndoTestAccess::setMateOpposed(window, true);
      const auto* opposedScene =
          MainWindowUndoTestAccess::matePreviewScene(window);
      a8aEvidence.alignedOpposedPreview =
          alignedScene && opposedScene && alignedScene != opposedScene &&
          MainWindowUndoTestAccess::canApplyMate(window);
      MainWindowUndoTestAccess::failMatePreview(window);
      MainWindowUndoTestAccess::setMateOpposed(window, false);
      a8aEvidence.failedPreviewRetentionAndDisabledApply =
          MainWindowUndoTestAccess::mateState(window) ==
              AssemblyMateToolState::PreviewFailed &&
          MainWindowUndoTestAccess::matePreviewScene(window) == opposedScene &&
          !MainWindowUndoTestAccess::mateApplyVisibleAndEnabled(window);
      MainWindowUndoTestAccess::setMateOpposed(window, true);

      bool controlsFit = true;
      const std::array<QSize, 4> a8aLogicalSizes{
          QSize{1920, 1080}, QSize{1536, 864}, QSize{1280, 720},
          QSize{960, 540}};
      for (const QSize logicalSize : a8aLogicalSizes) {
        window.resize(logicalSize);
        application.processEvents();
        const bool caseFit =
            MainWindowUndoTestAccess::mateControlsHaveValidGeometry(window);
        std::cout << "A8A_LAYOUT_CASE=" << logicalSize.width() << 'x'
                  << logicalSize.height() << ",actual=" << window.width()
                  << 'x' << window.height() << ",dpr="
                  << window.devicePixelRatioF() << ",controls="
                  << (caseFit ? "PASS" : "FAIL") << '\n';
        controlsFit = controlsFit && caseFit;
      }
      a8aEvidence.layoutDpr = controlsFit;

      const auto countBefore = MainWindowUndoTestAccess::mateCount(window);
      const auto historyBefore =
          MainWindowUndoTestAccess::history(window).size();
      MainWindowUndoTestAccess::applyMate(window);
      const bool applied =
          MainWindowUndoTestAccess::mateCount(window) == countBefore + 1 &&
          MainWindowUndoTestAccess::history(window).size() == historyBefore + 1;
      MainWindowUndoTestAccess::undo(window);
      const bool undone =
          MainWindowUndoTestAccess::mateCount(window) == countBefore;
      MainWindowUndoTestAccess::redo(window);
      const bool redone =
          MainWindowUndoTestAccess::mateCount(window) == countBefore + 1;
      a8aEvidence.oneUndoRedo = applied && undone && redone;

      MainWindowUndoTestAccess::beginMate(window);
      MainWindowUndoTestAccess::escapeMate(window);
      const bool escaped =
          MainWindowUndoTestAccess::mateState(window) ==
          AssemblyMateToolState::Inactive;
      MainWindowUndoTestAccess::beginMate(window);
      MainWindowUndoTestAccess::cancelMate(window);
      const bool cancelled =
          MainWindowUndoTestAccess::mateState(window) ==
          AssemblyMateToolState::Inactive;
      a8aEvidence.applyCancelEscape = applied && escaped && cancelled;

      MainWindowUndoTestAccess::beginMate(window);
      MainWindowUndoTestAccess::setMateReentryHook(window, [&window] {
        MainWindowUndoTestAccess::cancelMate(window);
        MainWindowUndoTestAccess::beginMate(window);
      });
      MainWindowUndoTestAccess::acceptMate(window, hits[0]);
      const bool reentered =
          MainWindowUndoTestAccess::mateState(window) ==
          AssemblyMateToolState::SelectingFirst;
      const bool replaced = window.loadProject(sharedAssemblyPath, &a8aError) &&
                            MainWindowUndoTestAccess::mateState(window) ==
                                AssemblyMateToolState::Inactive;
      a8aEvidence.teardownReentrancy = reentered && replaced;
    }
  }

  bool realGpu = false;
  std::string gpuReason = "viewport OpenGL context unavailable";
  window.resize(960, 540);
  application.processEvents();
  if (QOpenGLContext* context = viewport->context();
      viewport->isValid() && context && context->isValid()) {
    viewport->makeCurrent();
    auto* gl = context->functions();
    const auto text = [gl](GLenum name) {
      const auto* value = gl->glGetString(name);
      return value ? reinterpret_cast<const char*>(value) : "<unavailable>";
    };
    const std::string vendor = text(GL_VENDOR);
    const std::string renderer = text(GL_RENDERER);
    const std::string version = text(GL_VERSION);
    const std::string platform =
        QGuiApplication::platformName().toStdString();
    std::cout << "GL_VENDOR=" << vendor << '\n'
              << "GL_RENDERER=" << renderer << '\n'
              << "GL_VERSION=" << version << '\n'
              << "QT_PLATFORM=" << platform << '\n';
    const auto classification = test::classifyGpuSmokeRuntime(
        vendor, renderer, version, platform);
    realGpu = classification.realGpu;
    gpuReason = classification.reason;
    viewport->doneCurrent();
  }

  std::cout << "A5A_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A5B_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A5C1_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A5C2_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A5C3_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A5C4_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A5C5_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A5C6_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A5D_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A6B2_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A6C1_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  std::cout << "A8A_NATIVE_SCALE=" << window.devicePixelRatioF() << '\n';
  marker("A5A_OPEN_READ_ONLY", openReadOnly);
  marker("A5A_COMPLETE_SCENE_FIRST_COMMIT", completeFirstCommit);
  marker("A5A_TREE_TO_VIEWPORT_SELECTION", treeToViewport);
  marker("A5A_VIEWPORT_TO_TREE_SELECTION", viewportToTree);
  marker("A5A_SWITCH_PART_ASSEMBLY", partAssemblySwitch);
  marker("A5A_SWITCH_ASSEMBLY_ASSEMBLY", assemblyAssemblySwitch);
  marker("A5A_HIDDEN_SUPPRESSED", hiddenSuppressed);
  marker("A5A_TIMELINE_HIDDEN", timelineHidden);
  marker("A5A_NO_MUTATION_COMMANDS", noMutationCommands);
  marker("A5A_LAYOUT_MATRIX", layoutMatrix);
  a8aEvidence.realGpu = realGpu;
  marker("A8A_TYPED_TWO_STAGE", a8aEvidence.typedTwoStage);
  marker("A8A_OCCURRENCE_SCOPE", a8aEvidence.occurrenceScope);
  marker("A8A_STRICT_FILTER", a8aEvidence.strictFilter);
  marker("A8A_ALIGNED_OPPOSED_PREVIEW",
         a8aEvidence.alignedOpposedPreview);
  marker("A8A_FAILED_PREVIEW_RETENTION_DISABLED_APPLY",
         a8aEvidence.failedPreviewRetentionAndDisabledApply);
  marker("A8A_APPLY_CANCEL_ESCAPE", a8aEvidence.applyCancelEscape);
  marker("A8A_ONE_UNDO_REDO", a8aEvidence.oneUndoRedo);
  marker("A8A_TEARDOWN_REENTRANCY", a8aEvidence.teardownReentrancy);
  marker("A8A_LAYOUT_DPR", a8aEvidence.layoutDpr);
  const bool a8aGpuSmoke = test::validA8aGpuSmoke(a8aEvidence);
  marker("A8A_GPU_SMOKE", a8aGpuSmoke);
  marker("REAL_GPU", realGpu);

  const bool gpuSmoke = realGpu && openReadOnly && completeFirstCommit &&
                        treeToViewport && viewportToTree &&
                        partAssemblySwitch && assemblyAssemblySwitch &&
                        hiddenSuppressed && timelineHidden &&
                        noMutationCommands && layoutMatrix;
  marker("A5A_GPU_SMOKE", gpuSmoke);
  marker("A5B_NEW_ASSEMBLY", a5bNewAssembly);
  marker("INSERT_GHOST", insertGhost);
  marker("MOVE_ROTATE_GHOST", moveRotateGhost);
  marker("APPLY_ONE_UNDO", applyOneUndo);
  marker("CANCEL_EXACT_RESTORE", cancelExactRestore);
  marker("ESCAPE_NESTED_THEN_TOOL", escapeNestedThenTool);
  marker("FAILED_STAGE_PRESERVES_PREVIEW", failedStagePreservesPreview);
  marker("FAILED_OPEN_PRESERVES_PREVIEW", failedOpenPreservesPreview);
  marker("REPLACEMENT_TEARDOWN_ORDER", replacementTeardownOrder);
  marker("UNDO_REDO_STABLE_IDS", undoRedoStableIds);
  marker("HIDDEN_SUPPRESSED", hiddenSuppressed);
  marker("SELECTION_PUBLICATION_REFRESH", selectionPublicationRefresh);
  marker("LAYOUT_MATRIX", layoutMatrix);
  const bool a5bGpuSmoke = test::validA5bGpuSmoke(
      {realGpu, a5bNewAssembly, insertGhost, moveRotateGhost, applyOneUndo,
       cancelExactRestore, escapeNestedThenTool, failedStagePreservesPreview,
       failedOpenPreservesPreview, replacementTeardownOrder,
       undoRedoStableIds, hiddenSuppressed, selectionPublicationRefresh,
       layoutMatrix});
  marker("A5B_GPU_SMOKE", a5bGpuSmoke);
  marker("A5C1_ENTRYPOINTS", a5c1Entrypoints);
  marker("A5C1_APPLY_CANCEL", a5c1ApplyCancel);
  marker("A5C1_UNDO_REDO", a5c1UndoRedo);
  marker("A5C1_ONLY_NAME", a5c1OnlyName);
  marker("A5C1_DISABLED", a5c1Disabled);
  marker("A5C1_SELECTION_PUBLICATION_STABLE",
         a5c1SelectionPublicationStable);
  marker("A5C1_NO_UPLOAD", a5c1NoUpload);
  marker("A5C1_REPLACEMENT_TEARDOWN", a5c1ReplacementTeardown);
  marker("A5C1_LAYOUT_MATRIX", layoutMatrix);
  const bool a5c1GpuSmoke = test::validA5c1GpuSmoke(
      {realGpu, a5c1Entrypoints, a5c1ApplyCancel, a5c1UndoRedo,
       a5c1OnlyName, a5c1Disabled, a5c1SelectionPublicationStable,
       a5c1NoUpload, a5c1ReplacementTeardown, layoutMatrix});
  marker("A5C1_GPU_SMOKE", a5c1GpuSmoke);
  marker("A5C2_ENTRYPOINTS", a5c2Entrypoints);
  marker("A5C2_VISIBILITY_UNDO_REDO", a5c2VisibilityUndoRedo);
  marker("A5C2_ONLY_VISIBLE", a5c2OnlyVisible);
  marker("A5C2_HIDDEN_SPATIAL_EXCLUDED", a5c2HiddenSpatialExcluded);
  marker("A5C2_SHARED_SOURCE_NO_REMESH_UPLOAD",
         a5c2SharedSourceNoRemeshUpload);
  marker("A5C2_LAST_USER_PRUNE_REACQUIRE",
         a5c2LastUserPruneReacquire);
  marker("A5C2_ISOLATE_SHOW_ALL", a5c2IsolateShowAll);
  marker("A5C2_ISOLATE_PRESENTATION_ONLY",
         a5c2IsolatePresentationOnly);
  marker("A5C2_ISOLATE_NO_UPLOAD_PRUNE", a5c2IsolateNoUploadPrune);
  marker("A5C2_REPLACEMENT_TEARDOWN", a5c2ReplacementTeardown);
  marker("A5C2_LAYOUT_MATRIX", a5c2LayoutMatrix);
  const bool a5c2GpuSmoke = test::validA5c2GpuSmoke(
      {realGpu, a5c2Entrypoints, a5c2VisibilityUndoRedo, a5c2OnlyVisible,
       a5c2HiddenSpatialExcluded, a5c2SharedSourceNoRemeshUpload,
       a5c2LastUserPruneReacquire, a5c2IsolateShowAll,
       a5c2IsolatePresentationOnly, a5c2IsolateNoUploadPrune,
       a5c2ReplacementTeardown, a5c2LayoutMatrix});
  marker("A5C2_GPU_SMOKE", a5c2GpuSmoke);
  marker("A5C3_ENTRYPOINTS", a5c3Entrypoints);
  marker("A5C3_GROUND_UNGROUND", a5c3GroundUnground);
  marker("A5C3_UNDO_REDO", a5c3UndoRedo);
  marker("A5C3_ONLY_GROUNDED", a5c3OnlyGrounded);
  marker("A5C3_PLACEMENT_GUARD", a5c3PlacementGuard);
  marker("A5C3_VISIBILITY_SUPPRESSION_INDEPENDENT",
         a5c3VisibilitySuppressionIndependent);
  marker("A5C3_SELECTION_PUBLICATION_REFRESH",
         a5c3SelectionPublicationRefresh);
  marker("A5C3_NO_REMESH_UPLOAD", a5c3NoRemeshUpload);
  marker("A5C3_INSERT_CHAIN", a5c3InsertChain);
  marker("A5C3_ISOLATE_DISABLED", a5c3IsolateDisabled);
  marker("A5C3_REPLACEMENT_TEARDOWN", a5c3ReplacementTeardown);
  marker("A5C3_LAYOUT_MATRIX", a5c3LayoutMatrix);
  const bool a5c3GpuSmoke = test::validA5c3GpuSmoke(
      {realGpu, a5c3Entrypoints, a5c3GroundUnground, a5c3UndoRedo,
       a5c3OnlyGrounded, a5c3PlacementGuard,
       a5c3VisibilitySuppressionIndependent,
       a5c3SelectionPublicationRefresh, a5c3NoRemeshUpload,
       a5c3InsertChain, a5c3IsolateDisabled,
       a5c3ReplacementTeardown, a5c3LayoutMatrix});
  marker("A5C3_GPU_SMOKE", a5c3GpuSmoke);
  marker("A5C4_ENTRYPOINTS", a5c4Entrypoints);
  marker("A5C4_SUPPRESS_UNSUPPRESS", a5c4SuppressUnsuppress);
  marker("A5C4_UNDO_REDO", a5c4UndoRedo);
  marker("A5C4_ONLY_SUPPRESSED", a5c4OnlySuppressed);
  marker("A5C4_VISIBILITY_GROUNDED_INDEPENDENT",
         a5c4VisibilityGroundedIndependent);
  marker("A5C4_SUPPRESSED_SPATIAL_EXCLUDED",
         a5c4SuppressedSpatialExcluded);
  marker("A5C4_SHARED_SOURCE_NO_REMESH_UPLOAD",
         a5c4SharedSourceNoRemeshUpload);
  marker("A5C4_LAST_USER_PRUNE_REACQUIRE",
         a5c4LastUserPruneReacquire);
  marker("A5C4_SELECTION_PUBLICATION_REFRESH",
         a5c4SelectionPublicationRefresh);
  marker("A5C4_INSERT_CHAIN", a5c4InsertChain);
  marker("A5C4_ISOLATE_DISABLED", a5c4IsolateDisabled);
  marker("A5C4_REPLACEMENT_TEARDOWN", a5c4ReplacementTeardown);
  marker("A5C4_LAYOUT_MATRIX", a5c4LayoutMatrix);
  const bool a5c4GpuSmoke = test::validA5c4GpuSmoke(
      {realGpu, a5c4Entrypoints, a5c4SuppressUnsuppress, a5c4UndoRedo,
       a5c4OnlySuppressed, a5c4VisibilityGroundedIndependent,
       a5c4SuppressedSpatialExcluded, a5c4SharedSourceNoRemeshUpload,
       a5c4LastUserPruneReacquire, a5c4SelectionPublicationRefresh,
       a5c4InsertChain, a5c4IsolateDisabled, a5c4ReplacementTeardown,
       a5c4LayoutMatrix});
  marker("A5C4_GPU_SMOKE", a5c4GpuSmoke);
  marker("A5C5_ENTRYPOINTS", a5c5Entrypoints);
  marker("A5C5_NEW_STABLE_OCCURRENCE_ID", a5c5NewStableOccurrenceId);
  marker("A5C5_SAME_DEFINITION_DOCUMENT_BREP",
         a5c5SameDefinitionDocumentBrep);
  marker("A5C5_EXACT_PLACEMENT_NAME_FLAGS", a5c5ExactPlacementNameFlags);
  marker("A5C5_SELECTION_APPLY_UNDO_REDO",
         a5c5SelectionApplyUndoRedo);
  marker("A5C5_HIDDEN_SUPPRESSED_GROUNDED",
         a5c5HiddenSuppressedGrounded);
  marker("A5C5_SAME_EPOCH_PUBLICATION_REFRESH",
         a5c5SameEpochPublicationRefresh);
  marker("A5C5_SHARED_SOURCE_NO_REMESH_UPLOAD",
         a5c5SharedSourceNoRemeshUpload);
  marker("A5C5_INSTANCE_DRAW_PICKING",
         a5c5InstanceDraw && a5c5RectanglePickingBoth &&
             a5c5SharedSourceNoRemeshUpload);
  marker("A5C5_TOPOLOGY_REUSE", a5c5TopologyReuse);
  const bool a5c5InsertRenameIsolateDisabled =
      a5c5InsertDisabled && a5c5RenameDisabled && a5c5IsolateDisabled;
  marker("A5C5_INSERT_RENAME_ISOLATE_DISABLED",
         a5c5InsertRenameIsolateDisabled);
  marker("A5C5_FAILURE_ATOMICITY", a5c5FailureAtomicity);
  marker("A5C5_REPLACEMENT_TEARDOWN", a5c5ReplacementTeardown);
  marker("A5C5_LAYOUT_MATRIX", a5c5LayoutMatrix);
  const bool a5c5GpuSmoke = test::validA5c5GpuSmoke(
      {realGpu, a5c5Entrypoints, a5c5NewStableOccurrenceId,
       a5c5SameDefinitionDocumentBrep, a5c5ExactPlacementNameFlags,
       a5c5SelectionApplyUndoRedo, a5c5HiddenSuppressedGrounded,
       a5c5SameEpochPublicationRefresh, a5c5SharedSourceNoRemeshUpload,
       a5c5InstanceDraw, a5c5RectanglePickingBoth, a5c5TopologyReuse,
       a5c5InsertRenameIsolateDisabled,
       a5c5FailureAtomicity, a5c5ReplacementTeardown,
       a5c5LayoutMatrix});
  marker("A5C5_GPU_SMOKE", a5c5GpuSmoke);
  marker("A5C6_ENTRYPOINTS", a5c6Entrypoints);
  marker("A5C6_REMOVE_UNDO_REDO", a5c6RemoveUndoRedo);
  marker("A5C6_ONLY_OCCURRENCE_NODE", a5c6OnlyOccurrenceNode);
  marker("A5C6_ORPHAN_DEFINITION_RETAINED",
         a5c6OrphanDefinitionRetained);
  marker("A5C6_ORPHAN_TREE_FEEDBACK", a5c6OrphanTreeFeedback);
  marker("A5C6_LAST_OCCURRENCE_EMPTY_ROOT",
         a5c6LastOccurrenceEmptyRoot);
  marker("A5C6_HIDDEN_SUPPRESSED_GROUNDED",
         a5c6HiddenSuppressedGrounded);
  marker("A5C6_SELECTION_CLEAR_RESTORE", a5c6SelectionClearRestore);
  marker("A5C6_SAME_EPOCH_PUBLICATION_REFRESH",
         a5c6SameEpochPublicationRefresh);
  marker("A5C6_SHARED_SOURCE_NO_REMESH_UPLOAD",
         a5c6SharedSourceNoRemeshUpload);
  marker("A5C6_LAST_USER_PRUNE_REACQUIRE",
         a5c6LastUserPruneReacquire);
  marker("A5C6_INSTANCE_DRAW_PICKING", a5c6InstanceDrawPicking);
  marker("A5C6_UNDO_COLLISION_ROLLBACK", a5c6UndoCollisionRollback);
  marker("A5C6_INSERT_DUPLICATE_FIELDS_CHAIN",
         a5c6MixedChain);
  marker("A5C6_EMPTY_CAMERA", a5c6EmptyCamera);
  marker("A5C6_TOPOLOGY_PRUNE_REACQUIRE",
         a5c6TopologyPruneReacquire);
  marker("A5C6_ISOLATE_DISABLED", a5c6IsolateDisabled);
  marker("A5C6_FAILURE_ATOMICITY", a5c6FailureAtomicity);
  marker("A5C6_REPLACEMENT_TEARDOWN", a5c6ReplacementTeardown);
  marker("A5C6_LAYOUT_MATRIX", a5c6LayoutMatrix);
  const bool a5c6GpuSmoke = test::validA5c6GpuSmoke(
      {realGpu, a5c6Entrypoints, a5c6RemoveUndoRedo,
       a5c6OnlyOccurrenceNode, a5c6OrphanDefinitionRetained,
       a5c6OrphanTreeFeedback, a5c6LastOccurrenceEmptyRoot,
       a5c6HiddenSuppressedGrounded, a5c6SelectionClearRestore,
       a5c6SameEpochPublicationRefresh, a5c6SharedSourceNoRemeshUpload,
       a5c6LastUserPruneReacquire, a5c6InstanceDrawPicking,
       a5c6UndoCollisionRollback, a5c6MixedChain, a5c6EmptyCamera,
       a5c6TopologyPruneReacquire,
       a5c6IsolateDisabled, a5c6FailureAtomicity,
       a5c6ReplacementTeardown, a5c6LayoutMatrix});
  marker("A5C6_GPU_SMOKE", a5c6GpuSmoke);
  marker("A5D_ENTRY_RETURN_BREADCRUMB", a5dEntryReturnBreadcrumb);
  marker("A5D_ONE_OWNER_ONE_HISTORY", a5dOneOwnerOneHistory);
  marker("A5D_SHARED_EDIT_ALL_OCCURRENCES", a5dSharedEditAllOccurrences);
  marker("A5D_IDS_PLACEMENTS_FLAGS_STABLE", a5dIdsPlacementsFlagsStable);
  marker("A5D_FAILED_RECOMPUTE_ROLLBACK", a5dFailedRecomputeRollback);
  marker("A5D_FAILED_PROJECTION_ROLLBACK", a5dFailedProjectionRollback);
  marker("A5D_UNDO_REDO_SHARED", a5dUndoRedoShared);
  marker("A5D_TOOL_REPLACEMENT_TEARDOWN", a5dToolReplacementTeardown);
  marker("A5D_EDITED_SOURCE_ONE_UPLOAD", a5dEditedSourceOneUpload);
  marker("A5D_OTHER_SOURCES_REUSED", a5dOtherSourcesReused);
  marker("A5D_HIDDEN_SUPPRESSED_STABLE", a5dHiddenSuppressedStable);
  marker("A5D_SELECTION_CAMERA_RESTORE", a5dSelectionCameraRestore);
  marker("A5D_ORPHAN_DEFINITION_EDIT", a5dOrphanDefinitionEdit);
  marker("A5D_LAYOUT_MATRIX", a5dLayoutMatrix);
  const bool a5dGpuSmoke = test::validA5dGpuSmoke(
      {realGpu, a5dEntryReturnBreadcrumb, a5dOneOwnerOneHistory,
       a5dSharedEditAllOccurrences, a5dIdsPlacementsFlagsStable,
       a5dFailedRecomputeRollback, a5dFailedProjectionRollback,
       a5dUndoRedoShared, a5dToolReplacementTeardown,
       a5dEditedSourceOneUpload, a5dOtherSourcesReused,
       a5dHiddenSuppressedStable, a5dSelectionCameraRestore,
       a5dOrphanDefinitionEdit, a5dLayoutMatrix});
  marker("A5D_GPU_SMOKE", a5dGpuSmoke);
  const bool a6b2GpuSmoke = test::validA6b2GpuSmoke(
      {realGpu, a6b2InitialActive, a6b2Reload, a6b2ColdUndo, a6b2ColdRedo,
       a6b2StableFields, a6b2SelectionHover, a6b2SharedSource,
       a6b2SourceRetirement, a6b2OneUploadPerRevision,
       window.devicePixelRatioF() > 0.0});
  marker("A6B2_GPU_SMOKE", a6b2GpuSmoke);
  a6c1Evidence.realGpu = realGpu;
  a6c1Evidence.dpiIndependent =
      a6c1Evidence.dpiIndependent && window.devicePixelRatioF() > 0.0;
  const bool a6c1GpuSmoke = test::validA6c1GpuSmoke(a6c1Evidence);
  marker("A6C1_GPU_SMOKE", a6c1GpuSmoke);
  if (!realGpu && !gpuReason.empty())
    std::cerr << "REAL_GPU_REASON=" << gpuReason << '\n';
  return gpuSmoke && a5bGpuSmoke && a5c1GpuSmoke && a5c2GpuSmoke &&
                 a5c3GpuSmoke && a5c4GpuSmoke && a5c5GpuSmoke &&
                 a5c6GpuSmoke && a5dGpuSmoke && a6b2GpuSmoke &&
                 a6c1GpuSmoke && a8aGpuSmoke
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
