#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QTreeView>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/AppSettings.h"
#include "model/ImportedShapeFeature.h"
#include "project/ProjectFile.h"
#include "ui/AssemblyTreeModel.h"
#include "ui/MainWindow.h"
#include "ui/Viewport.h"

namespace solidar {

class MainWindowUndoTestAccess final {
 public:
  static bool createNewAssembly(MainWindow& window) {
    auto staged = window.projectApplicationService_.stageNewAssembly();
    if (!staged.succeeded()) return false;
    auto prepared = window.prepareAssemblyWorkspace(std::move(*staged.staged));
    if (!prepared) return false;
    window.commitAssemblyReplacement(std::move(prepared), QString{});
    return window.projectKind_ == EditorProjectKind::AssemblyReadOnly;
  }

  static bool insert(MainWindow& window, const QString& path) {
    const auto* root = window.productDocument_.rootAssemblyDefinition();
    if (!root || window.assemblyInsertController_.active()) return false;
    std::uint64_t definition = 1;
    while (window.productDocument_.definitions().contains(
        DefinitionId{definition}))
      ++definition;
    std::uint64_t occurrence = 1;
    while (root->find(OccurrenceId{occurrence})) ++occurrence;
    window.assemblyInsertController_.activate(
        window.assemblyInsertSession_, DefinitionId{definition},
        OccurrenceId{occurrence});
    auto staged = window.projectApplicationService_.stageEmbeddedPart(path);
    if (!staged.succeeded() ||
        !window.viewport_->prepareAssemblyPlacementPreview(
            *staged.staged->document) ||
        !window.assemblyInsertSession_.acceptSource(std::move(*staged.staged)))
      return false;
    window.resetAssemblyPlacementControls(true);
    window.applyAssemblyInsert();
    return !window.assemblyInsertController_.active();
  }

  static QModelIndex occurrenceIndex(const MainWindow& window, int row) {
    if (!window.assemblyTreeModel_) return {};
    const auto root = window.assemblyTreeModel_->index(0, 0);
    return window.assemblyTreeModel_->index(row, 0, root);
  }
  static void select(MainWindow& window, const QModelIndex& index) {
    window.assemblyTreeView_->setCurrentIndex(index);
  }
  static bool hoverSelected(MainWindow& window) {
    const auto& selected = window.viewport_->selectedAssemblyOccurrenceView();
    return selected && window.viewport_->assemblySession_ &&
           window.viewport_->assemblySession_->setHoveredOccurrence(selected);
  }
  static void toggleVisibility(MainWindow& window) {
    window.toggleAssemblyOccurrenceVisibility();
  }
  static void toggleGrounded(MainWindow& window) {
    window.toggleAssemblyOccurrenceGrounded();
  }
  static void toggleSuppressed(MainWindow& window) {
    window.toggleAssemblyOccurrenceSuppressed();
  }
  static void duplicate(MainWindow& window) {
    window.duplicateAssemblyOccurrence();
  }
  static void remove(MainWindow& window) {
    window.removeAssemblyOccurrence();
  }
  static void toggleIsolation(MainWindow& window) {
    window.toggleAssemblyIsolation();
  }
  static void undo(MainWindow& window) { window.undoLastAction(); }
  static void redo(MainWindow& window) { window.redoLastAction(); }
  static void injectProjection(MainWindow& window,
                               AssemblyViewportSessionFailurePoint point) {
    window.assemblyPreparationFailurePoint_ = point;
  }
  static void injectTreeResource(MainWindow& window, bool enabled) {
    window.assemblyTreeResourceFailureForTesting_ = enabled;
  }
  static void injectIsolation(MainWindow& window,
                              AssemblyIsolationFailurePoint point) {
    window.assemblyIsolationFailurePointForTesting_ = point;
  }

  static const ProductDocument& product(const MainWindow& window) {
    return window.productDocument_;
  }
  static const AssemblyCommandHistory& history(const MainWindow& window) {
    return window.assemblyCommandHistory_;
  }
  static const AssemblyViewportSession& session(const MainWindow& window) {
    CHECK(window.viewport_->assemblySession_);
    return *window.viewport_->assemblySession_;
  }
  static const AssemblyTreeModel* tree(const MainWindow& window) {
    return window.assemblyTreeModel_;
  }
  static std::optional<AssemblyOccurrenceSelection> selection(
      const MainWindow& window) {
    return window.viewport_->selectedAssemblyOccurrenceView();
  }
  static std::optional<AssemblyOccurrenceSelection> currentRow(
      const MainWindow& window) {
    if (!window.assemblyTreeModel_) return std::nullopt;
    const auto address = window.assemblyTreeModel_->addressForIndex(
        window.assemblyTreeView_->currentIndex());
    return address ? address->get().occurrence : std::nullopt;
  }
  static ViewportCameraState camera(const MainWindow& window) {
    return window.viewport_->assemblyCamera();
  }
  static RendererResourceDiagnostics renderer(const MainWindow& window) {
    return window.viewport_->rendererResourceDiagnostics();
  }
  static bool undoEnabled(const MainWindow& window) {
    return window.undoAction_->isEnabled();
  }
  static bool redoEnabled(const MainWindow& window) {
    return window.redoAction_->isEnabled();
  }
  static bool groundingEnabled(const MainWindow& window) {
    return window.assemblyGroundingAction_ &&
           window.assemblyGroundingAction_->isEnabled();
  }
  static bool suppressionEnabled(const MainWindow& window) {
    return window.assemblySuppressionAction_ &&
           window.assemblySuppressionAction_->isEnabled();
  }
  static bool duplicateEnabled(const MainWindow& window) {
    return window.assemblyDuplicateAction_ &&
           window.assemblyDuplicateAction_->isEnabled();
  }
  static bool removeEnabled(const MainWindow& window) {
    return window.assemblyRemoveAction_ &&
           window.assemblyRemoveAction_->isEnabled();
  }
};

}  // namespace solidar

namespace {

using namespace solidar;
using Access = MainWindowUndoTestAccess;

struct OccurrenceState final {
  const ComponentOccurrence* address{};
  OccurrenceId id;
  DefinitionId target;
  RigidTransform3d placement{RigidTransform3d::identity()};
  OccurrenceFlags flags{};
  std::string name;
  friend bool operator==(const OccurrenceState&,
                         const OccurrenceState&) = default;
};

struct ProductState final {
  const ProductDocument* address{};
  ProductIncarnationId incarnation;
  DefinitionId root;
  const AssemblyDefinition* rootAddress{};
  std::vector<DefinitionId> definitions;
  std::vector<const PartDefinition*> parts;
  std::vector<const Document*> documents;
  std::vector<std::string> documentFingerprints;
  std::vector<OccurrenceState> occurrences;
  friend bool operator==(const ProductState&, const ProductState&) = default;
};

ProductState productState(const ProductDocument& product) {
  ProductState state;
  state.address = &product;
  state.incarnation = product.incarnation();
  state.root = product.rootDefinitionId();
  state.rootAddress = product.rootAssemblyDefinition();
  state.definitions = product.definitions().definitionIds();
  for (const auto id : state.definitions) {
    if (const auto* part = product.definitions().findPart(id)) {
      state.parts.push_back(part);
      state.documents.push_back(&part->document());
      std::string fingerprint;
      for (const auto& body : part->document().bodies()) {
        fingerprint.append(std::to_string(body.id())).append(":");
        fingerprint.append(body.name()).append(":");
        fingerprint.push_back(body.visible() ? '1' : '0');
        fingerprint.push_back('|');
        for (const auto& feature : body.features()) {
          fingerprint.append(std::to_string(feature->id())).append(":");
          fingerprint.append(feature->name()).append(":");
          fingerprint.append(
              std::to_string(static_cast<int>(feature->state())));
          fingerprint.append(":");
          fingerprint.append(std::to_string(feature->shapeRevision()));
          fingerprint.append(":");
          fingerprint.append(std::to_string(reinterpret_cast<std::uintptr_t>(
              feature->lastValidShape().get())));
          fingerprint.push_back(';');
        }
      }
      state.documentFingerprints.push_back(std::move(fingerprint));
    }
  }
  for (const auto& [id, occurrence] : state.rootAddress->occurrences()) {
    state.occurrences.push_back(
        {&occurrence, id, occurrence.targetDefinitionId(),
         occurrence.placement(), occurrence.flags(),
         occurrence.displayNameUtf8()});
  }
  return state;
}

struct HistoryState final {
  std::size_t size{};
  std::size_t cursor{};
  std::size_t bytes{};
  AssemblyCommandRevision current;
  AssemblyCommandRevision savepoint;
  bool dirty{};
  bool busy{};
  bool canUndo{};
  bool canRedo{};
  std::optional<OccurrencePath> semanticSelection;
  friend bool operator==(const HistoryState&, const HistoryState&) = default;
};

HistoryState historyState(const AssemblyCommandHistory& history) {
  return {history.size(),
          history.cursor(),
          history.retainedBytes(),
          history.currentRevision(),
          history.savepointRevision(),
          !history.isAtSavepoint(),
          history.isBusy(),
          history.canUndo(),
          history.canRedo(),
          history.currentOccurrence()};
}

struct ProjectionState final {
  const AssemblyViewportSession* session{};
  const AssemblyTreeModel* tree{};
  const AssemblyViewportScene* scene{};
  ProductPresentationEpoch epoch;
  AssemblyScenePublicationGeneration publication;
  std::optional<AssemblyOccurrenceSelection> selection;
  std::optional<AssemblyOccurrenceSelection> row;
  std::optional<AssemblyOccurrenceIdentity> selectedHighlight;
  std::optional<AssemblyOccurrenceIdentity> hoveredHighlight;
  ViewportCameraState camera;
  RendererResourceDiagnostics renderer;
  AssemblyViewportSceneDiagnostics sceneDiagnostics;
  AssemblyViewportPickingDiagnostics pickingDiagnostics;
  bool modified{};
  bool undoEnabled{};
  bool redoEnabled{};
  bool isolated{};
};

bool sameCamera(const ViewportCameraState& left,
                const ViewportCameraState& right) {
  return left.yawDeg == right.yawDeg && left.pitchDeg == right.pitchDeg &&
         left.zoom == right.zoom && left.pan == right.pan &&
         left.logicalSize == right.logicalSize &&
         left.devicePixelRatio == right.devicePixelRatio &&
         left.center.x == right.center.x && left.center.y == right.center.y &&
         left.center.z == right.center.z &&
         left.depthExtent == right.depthExtent;
}

bool sameRenderer(const RendererResourceDiagnostics& left,
                  const RendererResourceDiagnostics& right) {
  return left.initialized == right.initialized &&
         left.sourceMeshes == right.sourceMeshes &&
         left.previewResident == right.previewResident &&
         left.cutPreviewResident == right.cutPreviewResident &&
         left.selectedFaces == right.selectedFaces &&
         left.hoveredFaces == right.hoveredFaces &&
         left.selectedEdges == right.selectedEdges &&
         left.hoveredEdges == right.hoveredEdges &&
         left.previewUploadGeneration == right.previewUploadGeneration &&
         left.cutPreviewUploadGeneration ==
             right.cutPreviewUploadGeneration &&
         left.previewContentHash == right.previewContentHash &&
         left.cutPreviewContentHash == right.cutPreviewContentHash &&
         left.previewIndexCount == right.previewIndexCount &&
         left.cutPreviewIndexCount == right.cutPreviewIndexCount &&
         left.highlightFaceIndexVisits == right.highlightFaceIndexVisits &&
         left.highlightEdgeVertexVisits == right.highlightEdgeVertexVisits &&
         left.highlightLookupBytes == right.highlightLookupBytes &&
         left.ordinarySourceMeshes == right.ordinarySourceMeshes &&
         left.highlightOnlySourceMeshes == right.highlightOnlySourceMeshes &&
         left.highlightOverlayMeshes == right.highlightOverlayMeshes &&
         left.sourceUploadAttempts == right.sourceUploadAttempts &&
         left.sourceUploadSuccesses == right.sourceUploadSuccesses &&
         left.sourceUploadFailures == right.sourceUploadFailures &&
         left.sourceDestroyCount == right.sourceDestroyCount &&
         left.sourcePruneCount == right.sourcePruneCount &&
         left.deferredSourceDestructions ==
             right.deferredSourceDestructions &&
         left.deferredSourceCleanupCount ==
             right.deferredSourceCleanupCount &&
         left.staleAssemblyRejections == right.staleAssemblyRejections &&
         left.activeAssemblyUsers == right.activeAssemblyUsers &&
         left.activePublicationGeneration ==
             right.activePublicationGeneration &&
         left.submittedAssemblyInstances ==
             right.submittedAssemblyInstances &&
         left.drawnAssemblyInstances == right.drawnAssemblyInstances &&
         left.highlightedAssemblyInstances ==
             right.highlightedAssemblyInstances &&
         left.drawnAssemblySurfacePasses ==
             right.drawnAssemblySurfacePasses &&
         left.drawnAssemblyEdgePasses == right.drawnAssemblyEdgePasses &&
         left.assemblyWorldBoundsValid == right.assemblyWorldBoundsValid &&
         left.assemblyCameraCenter.x == right.assemblyCameraCenter.x &&
         left.assemblyCameraCenter.y == right.assemblyCameraCenter.y &&
         left.assemblyCameraCenter.z == right.assemblyCameraCenter.z &&
         left.assemblyCameraDepthExtent == right.assemblyCameraDepthExtent &&
         left.sources.size() == right.sources.size() &&
         std::equal(left.sources.begin(), left.sources.end(),
                    right.sources.begin(), [](const auto& a, const auto& b) {
                      return a.key == b.key &&
                             a.uploadGeneration == b.uploadGeneration &&
                             a.contentFingerprint == b.contentFingerprint;
                    });
}

bool sameSceneDiagnostics(const AssemblyViewportSceneDiagnostics& left,
                          const AssemblyViewportSceneDiagnostics& right) {
  return left.publicationAttempts == right.publicationAttempts &&
         left.publicationSuccesses == right.publicationSuccesses &&
         left.publicationFailures == right.publicationFailures &&
         left.staleRejections == right.staleRejections &&
         left.meshBuildAttempts == right.meshBuildAttempts &&
         left.meshBuildSuccesses == right.meshBuildSuccesses &&
         left.meshBuildFailures == right.meshBuildFailures &&
         left.residentSources == right.residentSources &&
         left.activeSources == right.activeSources &&
         left.activeInstances == right.activeInstances &&
         left.activeGeneration == right.activeGeneration;
}

bool samePickingDiagnostics(const AssemblyViewportPickingDiagnostics& left,
                            const AssemblyViewportPickingDiagnostics& right) {
  return left.publicationAttempts == right.publicationAttempts &&
         left.publicationSuccesses == right.publicationSuccesses &&
         left.publicationFailures == right.publicationFailures &&
         left.stalePublicationRejections == right.stalePublicationRejections &&
         left.topologySourceBuildAttempts ==
             right.topologySourceBuildAttempts &&
         left.topologySourceBuildSuccesses ==
             right.topologySourceBuildSuccesses &&
         left.topologySourceBuildFailures ==
             right.topologySourceBuildFailures &&
         left.topologySourceReuses == right.topologySourceReuses &&
         left.v2FaceReferences == right.v2FaceReferences &&
         left.v1FaceFallbacks == right.v1FaceFallbacks &&
         left.v2EdgeReferences == right.v2EdgeReferences &&
         left.v1EdgeFallbacks == right.v1EdgeFallbacks &&
         left.spatialBuilds == right.spatialBuilds &&
         left.faceQueries == right.faceQueries &&
         left.edgeQueries == right.edgeQueries &&
         left.rectangleQueries == right.rectangleQueries &&
         left.snapQueries == right.snapQueries &&
         left.queryFailures == right.queryFailures &&
         left.staleQueryRejections == right.staleQueryRejections &&
         left.uncertainQueries == right.uncertainQueries &&
         left.projectedBuilds == right.projectedBuilds &&
         left.projectedReuses == right.projectedReuses &&
         left.broadPhaseCandidates == right.broadPhaseCandidates &&
         left.triangleTests == right.triangleTests &&
         left.edgeSegmentTests == right.edgeSegmentTests &&
         left.occluderTests == right.occluderTests &&
         left.residentTopologySources == right.residentTopologySources &&
         left.activeSources == right.activeSources &&
         left.activeInstances == right.activeInstances &&
         left.activeGeneration == right.activeGeneration;
}

ProjectionState projectionState(const MainWindow& window) {
  const auto& session = Access::session(window);
  const auto selected = session.selectedHighlight()
      ? std::optional<AssemblyOccurrenceIdentity>(
            session.selectedHighlight()->occurrence())
      : std::nullopt;
  const auto hovered = session.hoveredHighlight()
      ? std::optional<AssemblyOccurrenceIdentity>(
            session.hoveredHighlight()->occurrence())
      : std::nullopt;
  return {&session,
          Access::tree(window),
          session.scene().get(),
          session.epoch(),
          session.publicationHandle().generation(),
          Access::selection(window),
          Access::currentRow(window),
          selected,
          hovered,
          Access::camera(window),
          Access::renderer(window),
          session.sceneDiagnostics(),
          session.picking().diagnostics(),
          window.isWindowModified(),
          Access::undoEnabled(window),
          Access::redoEnabled(window),
          session.isolationActive()};
}

bool sameProjection(const ProjectionState& left,
                    const ProjectionState& right) {
  return left.session == right.session && left.tree == right.tree &&
         left.scene == right.scene && left.epoch == right.epoch &&
         left.publication == right.publication &&
         left.selection == right.selection && left.row == right.row &&
         left.selectedHighlight == right.selectedHighlight &&
         left.hoveredHighlight == right.hoveredHighlight &&
         sameCamera(left.camera, right.camera) &&
         sameRenderer(left.renderer, right.renderer) &&
         sameSceneDiagnostics(left.sceneDiagnostics, right.sceneDiagnostics) &&
         samePickingDiagnostics(left.pickingDiagnostics,
                                right.pickingDiagnostics) &&
         left.modified == right.modified &&
         left.undoEnabled == right.undoEnabled &&
         left.redoEnabled == right.redoEnabled &&
         left.isolated == right.isolated;
}

void checkExact(const MainWindow& window, const ProductState& product,
                const HistoryState& history,
                const ProjectionState& projection) {
  CHECK(productState(Access::product(window)) == product);
  CHECK(historyState(Access::history(window)) == history);
  CHECK(sameProjection(projectionState(window), projection));
}

void checkProjectionFailure(MainWindow& window,
                            AssemblyViewportSessionFailurePoint point,
                            void (*operation)(MainWindow&)) {
  const auto product = productState(Access::product(window));
  const auto history = historyState(Access::history(window));
  const auto projection = projectionState(window);
  Access::injectProjection(window, point);
  operation(window);
  Access::injectProjection(window, AssemblyViewportSessionFailurePoint::None);
  checkExact(window, product, history, projection);
}

void checkTreeFailure(MainWindow& window, void (*operation)(MainWindow&)) {
  const auto product = productState(Access::product(window));
  const auto history = historyState(Access::history(window));
  const auto projection = projectionState(window);
  Access::injectTreeResource(window, true);
  operation(window);
  Access::injectTreeResource(window, false);
  checkExact(window, product, history, projection);
}

void visibility(MainWindow& window) { Access::toggleVisibility(window); }
void grounding(MainWindow& window) { Access::toggleGrounded(window); }
void suppression(MainWindow& window) { Access::toggleSuppressed(window); }
void duplicate(MainWindow& window) { Access::duplicate(window); }
void removeOccurrence(MainWindow& window) { Access::remove(window); }
void undo(MainWindow& window) { Access::undo(window); }
void redo(MainWindow& window) { Access::redo(window); }

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  QTemporaryDir temporary(QDir::current().filePath(
      QStringLiteral("assembly-visibility-ui-failure-XXXXXX")));
  CHECK(temporary.isValid());
  AppSettings settings(temporary.filePath(QStringLiteral("settings.ini")));
  const QString partPath = temporary.filePath(QStringLiteral("part.solidar"));
  Document document;
  auto& body = document.addBody("body");
  body.addFeature(std::make_unique<ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()),
      "box"));
  CHECK(document.recompute());
  QString error;
  CHECK(project::ProjectFile::saveDocument(
      partPath, document, {project::ProjectPersistenceFormat::V4}, &error));

  MainWindow window(settings);
  CHECK(Access::createNewAssembly(window));
  CHECK(Access::insert(window, partPath));
  CHECK(Access::insert(window, partPath));
  Access::select(window, Access::occurrenceIndex(window, 0));
  CHECK(Access::selection(window));
  CHECK(Access::hoverSelected(window));

  constexpr std::array projectionFailures{
      AssemblyViewportSessionFailurePoint::Presentation,
      AssemblyViewportSessionFailurePoint::Scene,
      AssemblyViewportSessionFailurePoint::Picking,
      AssemblyViewportSessionFailurePoint::Spatial,
      AssemblyViewportSessionFailurePoint::Selection,
      AssemblyViewportSessionFailurePoint::Resource,
      AssemblyViewportSessionFailurePoint::FinalPreparation};
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, visibility);
  checkTreeFailure(window, visibility);

  visibility(window);
  CHECK(!Access::product(window)
             .rootAssemblyDefinition()
             ->find(Access::selection(window)->occurrence.path.entries().front())
             ->flags()
             .visible);
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, undo);
  checkTreeFailure(window, undo);

  undo(window);
  CHECK(Access::history(window).canRedo());
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, redo);
  checkTreeFailure(window, redo);

  // Isolate failure is an exact no-op both when entering and when an existing
  // isolate would be removed.
  constexpr std::array isolationFailures{
      AssemblyIsolationFailurePoint::Scene,
      AssemblyIsolationFailurePoint::Picking,
      AssemblyIsolationFailurePoint::Resource};
  for (const auto point : isolationFailures) {
    const auto product = productState(Access::product(window));
    const auto history = historyState(Access::history(window));
    const auto projection = projectionState(window);
    Access::injectIsolation(window, point);
    Access::toggleIsolation(window);
    Access::injectIsolation(window, AssemblyIsolationFailurePoint::None);
    checkExact(window, product, history, projection);
  }
  checkTreeFailure(window, [](MainWindow& value) {
    Access::toggleIsolation(value);
  });
  Access::toggleIsolation(window);
  CHECK(Access::session(window).isolationActive());
  for (const auto point : isolationFailures) {
    const auto product = productState(Access::product(window));
    const auto history = historyState(Access::history(window));
    const auto projection = projectionState(window);
    Access::injectIsolation(window, point);
    Access::toggleIsolation(window);
    Access::injectIsolation(window, AssemblyIsolationFailurePoint::None);
    checkExact(window, product, history, projection);
    CHECK(Access::session(window).isolationActive());
  }
  checkTreeFailure(window, [](MainWindow& value) {
    Access::toggleIsolation(value);
  });
  CHECK(Access::session(window).isolationActive());

  // Grounding uses the same fully transactional occurrence projection seam,
  // but is conservatively unavailable while isolate is active.
  CHECK(!Access::groundingEnabled(window));
  CHECK(!Access::suppressionEnabled(window));
  Access::toggleIsolation(window);
  CHECK(!Access::session(window).isolationActive());
  CHECK(Access::groundingEnabled(window));
  CHECK(Access::suppressionEnabled(window));
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, grounding);
  checkTreeFailure(window, grounding);

  grounding(window);
  const auto selectedId =
      Access::selection(window)->occurrence.path.entries().front();
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->find(selectedId)
            ->flags()
            .grounded);
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, undo);
  checkTreeFailure(window, undo);

  undo(window);
  CHECK(!Access::product(window)
             .rootAssemblyDefinition()
             ->find(selectedId)
             ->flags()
             .grounded);
  CHECK(Access::history(window).canRedo());
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, redo);
  checkTreeFailure(window, redo);
  redo(window);
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->find(selectedId)
            ->flags()
            .grounded);

  // Suppression has its own typed projection, but shares the same exact
  // precommit failure matrix. Hidden/grounded authored bits are preserved.
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, suppression);
  checkTreeFailure(window, suppression);
  const auto beforeSuppressionFlags = Access::product(window)
                                          .rootAssemblyDefinition()
                                          ->find(selectedId)
                                          ->flags();
  suppression(window);
  const auto suppressedFlags = Access::product(window)
                                   .rootAssemblyDefinition()
                                   ->find(selectedId)
                                   ->flags();
  CHECK(suppressedFlags.suppressed);
  CHECK(suppressedFlags.visible == beforeSuppressionFlags.visible);
  CHECK(suppressedFlags.grounded == beforeSuppressionFlags.grounded);
  CHECK(Access::selection(window));
  CHECK(!Access::session(window).selectedHighlight());
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, undo);
  checkTreeFailure(window, undo);

  undo(window);
  CHECK(!Access::product(window)
             .rootAssemblyDefinition()
             ->find(selectedId)
             ->flags()
             .suppressed);
  CHECK(Access::history(window).canRedo());
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, redo);
  checkTreeFailure(window, redo);
  redo(window);
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->find(selectedId)
            ->flags()
            .suppressed);

  // A5c5 Duplicate has the same prepare-before-commit guarantee for every
  // viewport/tree failure seam, on Execute, Undo, and Redo.  Return to an
  // effective source first; the source remains grounded, which is an allowed
  // authored state for duplication.
  undo(window);
  CHECK(!Access::product(window)
             .rootAssemblyDefinition()
             ->find(selectedId)
             ->flags()
             .suppressed);
  CHECK(Access::duplicateEnabled(window));
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, duplicate);
  checkTreeFailure(window, duplicate);

  const auto beforeDuplicateCount = Access::product(window)
                                        .rootAssemblyDefinition()
                                        ->occurrences()
                                        .size();
  duplicate(window);
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->occurrences()
            .size() == beforeDuplicateCount + 1);
  const auto duplicateSelection = Access::selection(window);
  CHECK(duplicateSelection);
  const auto duplicateId =
      duplicateSelection->occurrence.path.entries().front();
  CHECK(duplicateId != selectedId);
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, undo);
  checkTreeFailure(window, undo);

  undo(window);
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->occurrences()
            .size() == beforeDuplicateCount);
  CHECK(Access::history(window).canRedo());
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, redo);
  checkTreeFailure(window, redo);

  redo(window);
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->occurrences()
            .size() == beforeDuplicateCount + 1);
  CHECK(Access::selection(window));
  CHECK(Access::selection(window)->occurrence.path.entries().front() ==
        duplicateId);

  // A5c6 Remove executes, undoes, and redoes through the same unified
  // prepare-before-commit boundary. Every typed scene/tree failure restores
  // the entire product, command cursor/revisions, and immutable publication.
  CHECK(Access::removeEnabled(window));
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, removeOccurrence);
  checkTreeFailure(window, removeOccurrence);

  const auto beforeRemoveCount = Access::product(window)
                                     .rootAssemblyDefinition()
                                     ->occurrences()
                                     .size();
  removeOccurrence(window);
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->occurrences()
            .size() == beforeRemoveCount - 1);
  CHECK(!Access::selection(window));
  CHECK(!Access::session(window).selectedHighlight());
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, undo);
  checkTreeFailure(window, undo);

  undo(window);
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->occurrences()
            .size() == beforeRemoveCount);
  CHECK(Access::selection(window));
  CHECK(Access::selection(window)->occurrence.path.entries().front() ==
        duplicateId);
  CHECK(Access::history(window).canRedo());
  for (const auto point : projectionFailures)
    checkProjectionFailure(window, point, redo);
  checkTreeFailure(window, redo);

  redo(window);
  CHECK(Access::product(window)
            .rootAssemblyDefinition()
            ->occurrences()
            .size() == beforeRemoveCount - 1);
  CHECK(!Access::selection(window));
  return EXIT_SUCCESS;
}
