#include "TestAssertions.h"

#include <TopoDS_Shape.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QShortcut>
#include <QDoubleSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <type_traits>

#include "TestGeometryUtils.h"
#include "app/AppSettings.h"
#include "model/ExtrudeFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/FilletFeature.h"
#include "model/ChamferFeature.h"
#include "model/ShellFeature.h"
#include "model/DraftFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/MirrorFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/FilletToolSession.h"
#include "model/MoveFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"
#include "ui/MainWindow.h"
#include "ui/PartDesignHistory.h"
#include "ui/SketchCanvas.h"
#include "ui/ToolParametersPanel.h"
#include "ui/Viewport.h"
#include "ui/tools/PartDesignToolController.h"

namespace solidar {

class UnindexedPresentationFeature final : public ShapeFeature {
 public:
  explicit UnindexedPresentationFeature(ShapePtr shape)
      : ShapeFeature("Unindexed presentation fixture") {
    setShape(std::move(shape));
    markValid();
  }

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<UnindexedPresentationFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    markValid();
    return true;
  }
};

class MainWindowUndoTestAccess {
 public:
  inline static std::optional<MainWindow::HistorySelectionState>
      expectedEditUndoSelection_;
  static void reset(MainWindow& window) {
    window.modelHistory_.clear();
    window.updateUndoAvailability();
  }
  static bool pushBodyMarker(MainWindow& window, const std::string& name) {
    Document previous = window.document_;
    const auto previousSelection = window.captureHistorySelection();
    window.document_.addBody(name);
    if (!window.document_.recompute()) {
      window.document_ = std::move(previous);
      return false;
    }
    return window.pushModelTransition(std::move(previous),
                                      previousSelection);
  }
  static std::size_t undoCount(const MainWindow& window) {
    return window.modelHistory_.undoCount();
  }
  static std::size_t retainedBytes(const MainWindow& window) {
    return window.modelHistory_.retainedBytes();
  }
  static void setHistoryByteBudget(MainWindow& window, std::size_t bytes) {
    window.modelHistory_.setByteBudget(bytes);
  }
  static bool applyingUndo(const MainWindow& window) {
    return window.modelHistory_.isApplying();
  }
  static void pushThrowingUndo(MainWindow& window) {
    Document invalid = window.document_;
    Body* body = invalid.activeBody();
    if (!body) body = &invalid.addBody("failure fixture");
    auto broken = std::make_unique<MoveFeature>(
        FeatureId{987654321}, Vector3d{1.0, 0.0, 0.0},
        "undo failure fixture");
    const FeatureId brokenId = broken->id();
    body->addFeature(std::move(broken));
    static_cast<void>(invalid.recompute());
    Document current = invalid;
    const auto index = current.findBody(body->id())->featureIndex(brokenId);
    if (!index || !current.applyFeatureSlice(body->id(), *index, nullptr) ||
        !current.recompute())
      return;
    window.document_ = std::move(current);
    auto state = window.captureHistorySelection();
    static_cast<void>(window.modelHistory_.recordTransition(
        window.document_, std::move(invalid), state, state));
    window.updateUndoAvailability();
  }
  static std::pair<std::size_t, std::size_t> documentDeltaSize(
      const Document& before, const Document& after) {
    const auto metrics = ModelCommandHistory::inspectDelta(before, after);
    return {metrics.sliceCount, metrics.retainedBytes};
  }
  static Document documentCopy(const MainWindow& window) {
    return window.document_;
  }
  static bool importedHistoryIsColdAndRebuildable() {
    std::weak_ptr<const TopoDS_Shape> sourceWeak;
    std::weak_ptr<const TopologyIndex> topologyWeak;
    Document restored;
    ModelCommandHistory history;
    EditorCommittedState state;
    {
      Document before;
      auto& body = before.addBody("Imported heavy body");
      auto source = std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(30.0, 20.0, 10.0).Shape());
      sourceWeak = source;
      body.addFeature(std::make_unique<ImportedShapeFeature>(source, "Import"));
      source.reset();
      if (!before.recompute()) return false;
      const auto* imported = dynamic_cast<const ImportedShapeFeature*>(
          before.bodies().front().features().front().get());
      if (!imported || !imported->hasShape()) return false;
      std::string indexError;
      auto index = imported->lastValidTopologyIndex(&indexError);
      if (!index || !indexError.empty()) return false;
      topologyWeak = index;
      index.reset();
      const auto result = history.recordTransition(
          restored, std::move(before), state, state);
      if (result.status != HistoryCommitStatus::Accepted) return false;
    }
    if (!sourceWeak.expired() || !topologyWeak.expired()) return false;
    const auto undo = history.undo(restored, state);
    if (!undo.changed) return false;
    const auto* restoredImported = dynamic_cast<const ImportedShapeFeature*>(
        restored.bodies().front().features().front().get());
    return restoredImported && restoredImported->hasShape() &&
           restoredImported->importedShape() &&
           test::near(test::volumeOf(*restoredImported->shape()), 6000.0);
  }
  static void installDocument(MainWindow& window, Document document) {
    window.applyImportedDocument(std::move(document),
                                 QStringLiteral("history-matrix"));
    reset(window);
  }
  static void selectBody(MainWindow& window, BodyId id) {
    window.viewport_->setSelectedBodyEdges({});
    window.viewport_->setSelectedBodyFaces({});
    window.viewport_->setSelectedBodies({id});
    window.moveHistoryToEnd();
  }
  static bool selectSentinel(MainWindow& window, BodyId bodyId,
                             FeatureId featureId, int domain) {
    auto* feature = dynamic_cast<ShapeFeature*>(
        window.document_.findFeature(featureId));
    if (!feature || !feature->lastValidShape()) return false;
    window.viewport_->setSelectedBodies({});
    window.viewport_->setSelectedBodyEdges({});
    window.viewport_->setSelectedBodyFaces({});
    if (domain == 0) {
      window.viewport_->setSelectedBodies({bodyId});
    } else if (domain == 1) {
      window.viewport_->setSelectedBodyEdges({makeEdgeReference(
          *feature->lastValidShape(), bodyId, featureId, 0)});
    } else {
      window.viewport_->setSelectedBodyFaces({makeFaceReference(
          *feature->lastValidShape(), bodyId, featureId, 0)});
    }
    window.moveHistoryToEnd();
    return true;
  }
  static void clearSelection(MainWindow& window) {
    window.viewport_->setSelectedBodyEdges({});
    window.viewport_->setSelectedBodyFaces({});
    window.viewport_->setSelectedBodies({});
    window.moveHistoryToEnd();
  }
  static bool commitBodyRemoval(MainWindow& window, BodyId id) {
    Document previous = window.document_;
    const auto selection = window.captureHistorySelection();
    std::optional<std::size_t> index;
    for (std::size_t i = 0; i < window.document_.bodies().size(); ++i)
      if (window.document_.bodies()[i].id() == id) index = i;
    if (!index || !window.document_.applyBodySlice(*index, std::nullopt) ||
        !window.document_.recompute()) {
      window.document_ = std::move(previous);
      return false;
    }
    window.refreshBodyViewFromDocument();
    window.rebuildFeatureTree();
    window.rebuildHistoryPanel();
    return window.pushModelTransition(std::move(previous), selection);
  }
  static bool acceptFixtureThroughProduction(MainWindow& window,
                                             const Document& reference,
                                             FeatureId featureId) {
    const auto* target = reference.findFeature(featureId);
    if (!target) return false;
    window.resetTransientModelingUi();
    const auto bodyForFeature = [&window](FeatureId id) -> Body* {
      for (const auto& candidate : window.document_.bodies())
        if (candidate.featureIndex(id))
          return window.document_.findBody(candidate.id());
      return nullptr;
    };
    const auto source = [&window, &bodyForFeature](FeatureId id)
        -> std::pair<Body*, ShapeFeature*> {
      Body* body = bodyForFeature(id);
      return {body, body ? dynamic_cast<ShapeFeature*>(
                               window.document_.findFeature(id))
                         : nullptr};
    };

    if (const auto* value = dynamic_cast<const ExtrudeFeature*>(target)) {
      if (value->isFaceSource()) {
        const auto face = value->faceReference();
        if (!face) return false;
        const auto [body, sourceFeature] = source(face->featureId);
        if (!body || !sourceFeature) return false;
        window.partDesignCoordinator_.beginExtrude(
            body->id(), sourceFeature->id(), sourceFeature->shape(), *face,
            value->lengthMm(), value->operation(), value->reversed(),
            std::nullopt, sourceFeature->topologyIndex());
      } else {
        const auto* profile = window.document_.findSketch(value->profileSketchId());
        if (!profile) return false;
        window.partDesignCoordinator_.beginSketchExtrude(
            *profile, profile->id, {}, value->lengthMm(), value->operation(),
            value->reversed(), std::nullopt, value->profileOverride());
      }
      window.toolParametersPanel_->setParameterValue(value->lengthMm());
      window.updateFaceExtrudeToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptFaceExtrudeTool();
    } else if (const auto* value = dynamic_cast<const RevolveFeature*>(target)) {
      window.partDesignCoordinator_.beginRevolve(window.document_, kInvalidBodyId,
                                       kInvalidFeatureId, {});
      window.partDesignCoordinator_.setRevolveProfile(window.document_, value->profileSketchId(),
                                            value->profileOverride());
      window.partDesignCoordinator_.setRevolveAxis(window.document_, value->axis());
      window.partDesignCoordinator_.setRevolveAngleFromPanel(window.document_, value->angleDeg());
      window.partDesignCoordinator_.setRevolveOperation(window.document_, value->operation());
      window.partDesignCoordinator_.setRevolveReversed(window.document_, value->reversed());
      window.updateRevolveToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, window.document_).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptRevolveTool();
    } else if (const auto* value = dynamic_cast<const PocketFeature*>(target)) {
      QTimer::singleShot(0, [depth = value->depthMm()] {
        auto* dialog = qobject_cast<QInputDialog*>(
            QApplication::activeModalWidget());
        if (!dialog) return;
        const auto spins = dialog->findChildren<QDoubleSpinBox*>();
        if (!spins.empty()) spins.front()->setValue(depth);
        dialog->accept();
      });
      window.createPocket();
    } else if (const auto* value = dynamic_cast<const FilletFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->edge().featureId);
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginFillet(body->id(), sourceFeature->id(),
          sourceFeature->shape(), value->edges(), value->radiusMm(),
          std::nullopt, sourceFeature->topologyIndex());
      window.toolParametersPanel_->setParameterValue(value->radiusMm());
      window.updateFilletToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptFilletTool();
    } else if (const auto* value = dynamic_cast<const ChamferFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->edge().featureId);
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginChamfer(body->id(), sourceFeature->id(),
          sourceFeature->shape(), value->edges(), value->distanceMm(),
          std::nullopt, sourceFeature->topologyIndex());
      window.toolParametersPanel_->setParameterValue(value->distanceMm());
      window.updateChamferToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptChamferTool();
    } else if (const auto* value = dynamic_cast<const ShellFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginShell(body->id(), sourceFeature->id(),
          sourceFeature->shape(), value->removedFaces(), value->thicknessMm(),
          value->outside(), std::nullopt, sourceFeature->topologyIndex());
      window.toolParametersPanel_->setParameterValue(value->thicknessMm());
      window.updateShellToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptShellTool();
    } else if (const auto* value = dynamic_cast<const DraftFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginDraft(window.document_, body->id(),
          sourceFeature->id(), sourceFeature->shape(), value->draftedFaces(),
          value->neutralPlane(), value->pullDirection(), value->angleDeg(),
          value->reversed(), std::nullopt, value->rotationEdge(),
          sourceFeature->topologyIndex());
      window.updateDraftToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, window.document_).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptDraftTool();
    } else if (const auto* value = dynamic_cast<const JoinBodiesFeature*>(target)) {
      const auto [firstBody, firstFeature] = source(value->firstFeatureId());
      const auto [secondBody, secondFeature] = source(value->secondFeatureId());
      if (!firstBody || !secondBody || !firstFeature || !secondFeature)
        return false;
      window.partDesignCoordinator_.beginJoinBodies();
      window.partDesignCoordinator_.setJoinBodies({
          {firstBody->id(), firstFeature->id(), firstFeature->shape()},
          {secondBody->id(), secondFeature->id(), secondFeature->shape()}});
      window.updateJoinBodiesToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_ ||
          window.viewport_->toolPreviewReplacedBodyIds_.size() != 2)
        return false;
      window.acceptJoinBodiesTool();
    } else if (const auto* value = dynamic_cast<const MoveFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginMove(value->offsetMm());
      window.partDesignCoordinator_.setMoveBody(body->id(), sourceFeature->id(),
                                      sourceFeature->shape());
      window.updateMoveToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle != ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptMoveTool();
    } else if (const auto* value = dynamic_cast<const MirrorFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginMirror();
      window.partDesignCoordinator_.setMirrorBody(body->id(), sourceFeature->id(),
                                        sourceFeature->shape());
      window.partDesignCoordinator_.setMirrorPlane(value->plane());
      window.updateMirrorToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptMirrorTool();
    } else if (const auto* value =
                   dynamic_cast<const LinearPatternFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginLinearPattern(value->spacingMm(), value->count(),
                                             value->operation());
      window.partDesignCoordinator_.setLinearPatternBody(body->id(), sourceFeature->id(),
                                               sourceFeature->shape());
      window.partDesignCoordinator_.setLinearPatternDirection(value->direction());
      window.updateLinearPatternToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptLinearPatternTool();
    } else if (const auto* value =
                   dynamic_cast<const CircularPatternFeature*>(target)) {
      const auto [body, sourceFeature] = source(value->sourceFeatureId());
      if (!body || !sourceFeature) return false;
      window.partDesignCoordinator_.beginCircularPattern(value->angleDeg(), value->count(),
                                               value->operation());
      window.partDesignCoordinator_.setCircularPatternBody(body->id(), sourceFeature->id(),
                                                 sourceFeature->shape());
      window.partDesignCoordinator_.setCircularPatternAxis(value->axis());
      window.updateCircularPatternToolPreview();
      if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle !=
              ToolLifecycle::PreviewValid ||
          !window.viewport_->toolPreviewShape_)
        return false;
      window.acceptCircularPatternTool();
    } else {
      return false;
    }
    if (!window.modelHistory_.canUndo()) {
      if (window.modelHistory_.byteBudget() > 1U)
        std::cerr << "production accept failed for kind "
                  << static_cast<int>(target->kind())
                  << ": fillet=" << window.partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).error
                  << " filletState="
                  << static_cast<int>(window.partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle)
                  << " chamfer=" << window.partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).error
                  << " shell=" << window.partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).error
                  << " draft=" << window.partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, window.document_).error
                  << '\n';
      return false;
    }
    return true;
  }
  static bool editFeatureThroughProduction(MainWindow& window, BodyId bodyId,
                                           FeatureId featureId) {
    const auto* feature = window.document_.findFeature(featureId);
    if (!feature) return false;
    const auto oldUndoCount = window.modelHistory_.undoCount();
    expectedEditUndoSelection_ = window.captureHistorySelection();
    const auto* extrudeFeature = dynamic_cast<const ExtrudeFeature*>(feature);
    if ((extrudeFeature && !extrudeFeature->isFaceSource()) ||
        dynamic_cast<const PocketFeature*>(feature)) {
      QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        const auto spins = dialog->findChildren<QDoubleSpinBox*>();
        if (!spins.empty()) spins.front()->setValue(
            dynamic_cast<QInputDialog*>(dialog) ? 7.0 : 25.0);
        dialog->accept();
      });
      window.editHistoryFeature(bodyId, featureId);
      return window.modelHistory_.undoCount() == oldUndoCount + 1;
    }

    window.editHistoryFeature(bodyId, featureId);
    if (extrudeFeature) {
      window.partDesignCoordinator_.setExtrudeLengthFromPanel(25.0);
      window.toolParametersPanel_->setParameterValue(25.0);
      window.acceptFaceExtrudeTool();
    } else if (dynamic_cast<const RevolveFeature*>(feature)) {
      window.partDesignCoordinator_.setRevolveAngleFromPanel(window.document_, 180.0);
      window.acceptRevolveTool();
    } else if (dynamic_cast<const FilletFeature*>(feature)) {
      window.partDesignCoordinator_.setFilletRadiusFromPanel(1.0);
      window.toolParametersPanel_->setParameterValue(1.0);
      window.acceptFilletTool();
    } else if (dynamic_cast<const ChamferFeature*>(feature)) {
      window.partDesignCoordinator_.setChamferDistanceFromPanel(0.5);
      window.toolParametersPanel_->setParameterValue(0.5);
      window.acceptChamferTool();
    } else if (dynamic_cast<const ShellFeature*>(feature)) {
      window.partDesignCoordinator_.setShellThicknessFromPanel(1.0);
      window.toolParametersPanel_->setParameterValue(1.0);
      window.acceptShellTool();
    } else if (dynamic_cast<const DraftFeature*>(feature)) {
      window.partDesignCoordinator_.setDraftAngleFromPanel(window.document_, 3.0);
      window.acceptDraftTool();
    } else if (const auto* join =
                   dynamic_cast<const JoinBodiesFeature*>(feature)) {
      const Body* replacementBody = nullptr;
      const ShapeFeature* replacementFeature = nullptr;
      for (const auto& body : window.document_.bodies()) {
        if (body.id() == bodyId || body.id() == join->firstBodyId() ||
            body.id() == join->secondBodyId() || !body.activeFeature())
          continue;
        replacementBody = &body;
        replacementFeature = body.activeFeature();
        break;
      }
      const Body* firstBody = window.document_.findBody(join->firstBodyId());
      const auto* firstFeature = dynamic_cast<const ShapeFeature*>(
          window.document_.findFeature(join->firstFeatureId()));
      if (!replacementBody || !replacementFeature || !firstBody ||
          !firstFeature)
        return false;
      window.partDesignCoordinator_.setJoinBodies({
          {firstBody->id(), firstFeature->id(), firstFeature->lastValidShape()},
          {replacementBody->id(), replacementFeature->id(),
           replacementFeature->lastValidShape()}});
      window.acceptJoinBodiesTool();
    } else if (dynamic_cast<const MoveFeature*>(feature)) {
      window.partDesignCoordinator_.setMoveOffset({7.0, 1.0, 0.0});
      window.acceptMoveTool();
    } else if (dynamic_cast<const MirrorFeature*>(feature)) {
      window.partDesignCoordinator_.setMirrorPlane(MirrorPlane::XZ);
      window.acceptMirrorTool();
    } else if (dynamic_cast<const LinearPatternFeature*>(feature)) {
      window.partDesignCoordinator_.setLinearPatternCount(2);
      window.partDesignCoordinator_.setLinearPatternSpacing(50.0);
      window.acceptLinearPatternTool();
    } else if (dynamic_cast<const CircularPatternFeature*>(feature)) {
      window.partDesignCoordinator_.setCircularPatternCount(3);
      window.partDesignCoordinator_.setCircularPatternAngle(270.0);
      window.acceptCircularPatternTool();
    } else {
      return false;
    }
    return window.modelHistory_.undoCount() == oldUndoCount + 1;
  }
  static bool acceptUnchangedFeatureEditThroughProduction(
      MainWindow& window, BodyId bodyId, FeatureId featureId) {
    const auto* feature = window.document_.findFeature(featureId);
    if (!feature) return false;
    const auto undoCount = window.modelHistory_.undoCount();
    const auto redoCount = window.modelHistory_.redoCount();
    const auto* extrude = dynamic_cast<const ExtrudeFeature*>(feature);
    if ((extrude && !extrude->isFaceSource()) ||
        dynamic_cast<const PocketFeature*>(feature)) {
      QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QDialog*>(
                QApplication::activeModalWidget()))
          dialog->accept();
      });
      window.editHistoryFeature(bodyId, featureId);
    } else {
      window.editHistoryFeature(bodyId, featureId);
      if (!window.editUiTransaction_) return false;
      if (extrude) window.acceptFaceExtrudeTool();
      else if (dynamic_cast<const RevolveFeature*>(feature))
        window.acceptRevolveTool();
      else if (dynamic_cast<const FilletFeature*>(feature))
        window.acceptFilletTool();
      else if (dynamic_cast<const ChamferFeature*>(feature))
        window.acceptChamferTool();
      else if (dynamic_cast<const ShellFeature*>(feature))
        window.acceptShellTool();
      else if (dynamic_cast<const DraftFeature*>(feature))
        window.acceptDraftTool();
      else if (dynamic_cast<const JoinBodiesFeature*>(feature))
        window.acceptJoinBodiesTool();
      else if (dynamic_cast<const MoveFeature*>(feature))
        window.acceptMoveTool();
      else if (dynamic_cast<const MirrorFeature*>(feature))
        window.acceptMirrorTool();
      else if (dynamic_cast<const LinearPatternFeature*>(feature))
        window.acceptLinearPatternTool();
      else if (dynamic_cast<const CircularPatternFeature*>(feature))
        window.acceptCircularPatternTool();
      else
        return false;
    }
    const bool sameUndo = window.modelHistory_.undoCount() == undoCount;
    const bool sameRedo = window.modelHistory_.redoCount() == redoCount;
    const bool noTransaction = !window.editUiTransaction_;
    const bool inactive = transientToolsInactive(window);
    if (!(sameUndo && sameRedo && noTransaction && inactive))
      std::cerr << "unchanged helper mismatch: undo " << sameUndo
                << " redo " << sameRedo << " tx " << noTransaction
                << " inactive " << inactive << '\n';
    return sameUndo && sameRedo && noTransaction && inactive;
  }
  static bool cancelFeatureEditThroughProduction(MainWindow& window,
                                                 BodyId bodyId,
                                                 FeatureId featureId) {
    const auto* feature = window.document_.findFeature(featureId);
    if (!feature) return false;
    const auto beforeSelection = window.captureHistorySelection();
    const auto undoCount = window.modelHistory_.undoCount();
    const auto* extrude = dynamic_cast<const ExtrudeFeature*>(feature);
    if ((extrude && !extrude->isFaceSource()) ||
        dynamic_cast<const PocketFeature*>(feature)) {
      QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QDialog*>(
                QApplication::activeModalWidget()))
          dialog->reject();
      });
      window.editHistoryFeature(bodyId, featureId);
    } else {
      window.editHistoryFeature(bodyId, featureId);
      if (extrude) window.cancelFaceExtrudeTool();
      else if (dynamic_cast<const RevolveFeature*>(feature))
        window.cancelRevolveTool();
      else if (dynamic_cast<const FilletFeature*>(feature))
        window.cancelFilletTool();
      else if (dynamic_cast<const ChamferFeature*>(feature))
        window.cancelChamferTool();
      else if (dynamic_cast<const ShellFeature*>(feature))
        window.cancelShellTool();
      else if (dynamic_cast<const DraftFeature*>(feature))
        window.cancelDraftTool();
      else if (dynamic_cast<const JoinBodiesFeature*>(feature))
        window.cancelJoinBodiesTool();
      else if (dynamic_cast<const MoveFeature*>(feature))
        window.cancelMoveTool();
      else if (dynamic_cast<const MirrorFeature*>(feature))
        window.cancelMirrorTool();
      else if (dynamic_cast<const LinearPatternFeature*>(feature))
        window.cancelLinearPatternTool();
      else if (dynamic_cast<const CircularPatternFeature*>(feature))
        window.cancelCircularPatternTool();
      else
        return false;
    }
    return window.modelHistory_.undoCount() == undoCount &&
           sameSelectionState(window.captureHistorySelection(),
                              beforeSelection) &&
           transientToolsInactive(window);
  }
  static bool editJoinOperands(MainWindow& window, BodyId ownerBodyId,
                               FeatureId joinId, BodyId firstBodyId,
                               BodyId secondBodyId) {
    const auto oldUndoCount = window.modelHistory_.undoCount();
    window.editHistoryFeature(ownerBodyId, joinId);
    const Body* first = window.document_.findBody(firstBodyId);
    const Body* second = window.document_.findBody(secondBodyId);
    if (!first || !second || !first->activeFeature() ||
        !second->activeFeature())
      return false;
    window.partDesignCoordinator_.setJoinBodies({
        {first->id(), first->activeFeature()->id(),
         first->activeFeature()->lastValidShape()},
        {second->id(), second->activeFeature()->id(),
         second->activeFeature()->lastValidShape()}});
    window.updateJoinBodiesToolPreview();
    if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle !=
        ToolLifecycle::PreviewValid)
      return false;
    window.acceptJoinBodiesTool();
    return window.modelHistory_.undoCount() == oldUndoCount + 1;
  }
  static bool cancelJoinEdit(MainWindow& window, BodyId ownerBodyId,
                             FeatureId joinId,
                             BodyId sharedOperand = kInvalidBodyId,
                             BodyId exclusiveOperand = kInvalidBodyId) {
    const auto payloadBefore = window.document_.findFeature(joinId);
    if (!payloadBefore) return false;
    const auto undoBefore = window.modelHistory_.undoCount();
    window.editHistoryFeature(ownerBodyId, joinId);
    if (window.partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle == ToolLifecycle::Inactive)
      return false;
    const auto presented = [&window](BodyId id) {
      return std::any_of(window.viewport_->bodyViewShapes_.begin(),
                         window.viewport_->bodyViewShapes_.end(),
                         [id](const BodyViewShape& shape) {
                           return shape.bodyId == id;
                         });
    };
    if (sharedOperand != kInvalidBodyId && presented(sharedOperand))
      return false;
    if (exclusiveOperand != kInvalidBodyId && !presented(exclusiveOperand))
      return false;
    window.cancelJoinBodiesTool();
    return window.modelHistory_.undoCount() == undoBefore &&
           window.document_.findFeature(joinId) != nullptr &&
           transientToolsInactive(window);
  }
  static bool editUndoSelectionRestored(const MainWindow& window) {
    if (!expectedEditUndoSelection_) return false;
    const auto current = window.captureHistorySelection();
    const auto& expected = *expectedEditUndoSelection_;
    return current.bodies == expected.bodies &&
           current.edges == expected.edges && current.faces == expected.faces &&
           current.historyPosition == expected.historyPosition &&
           current.atEnd == expected.atEnd &&
           current.historyBodyId == expected.historyBodyId &&
           current.historyFeatureId == expected.historyFeatureId &&
           current.historySketchId == expected.historySketchId;
  }
  static std::optional<MainWindow::HistorySelectionState>
  expectedEditUndoSelection() {
    return expectedEditUndoSelection_;
  }
  static bool removeFeatureThroughProduction(MainWindow& window,
                                             FeatureId featureId) {
    const auto found = std::find_if(
        window.historySteps_.begin(), window.historySteps_.end(),
        [featureId](const HistoryStep& step) {
          return step.featureId == featureId;
        });
    if (found == window.historySteps_.end()) return false;
    const HistoryStep step = *found;
    const auto oldUndoCount = window.modelHistory_.undoCount();
    QTimer::singleShot(0, [] {
      if (auto* box = qobject_cast<QMessageBox*>(
              QApplication::activeModalWidget()))
        box->done(QMessageBox::Yes);
    });
    window.removeHistoryStep(step);
    return window.modelHistory_.undoCount() == oldUndoCount + 1 &&
           !window.document_.findFeature(featureId);
  }
  static const ShapeFeature* feature(const MainWindow& window, FeatureId id) {
    return window.document_.findFeature(id);
  }
  static std::vector<FeatureId> featuresOfKind(const MainWindow& window,
                                               FeatureKind kind) {
    std::vector<FeatureId> result;
    for (const auto& body : window.document_.bodies())
      for (const auto& feature : body.features())
        if (feature && feature->kind() == kind) result.push_back(feature->id());
    std::sort(result.begin(), result.end());
    return result;
  }
  static bool selectedBody(const MainWindow& window, BodyId id) {
    return window.viewport_->selectedBodies() == std::vector<BodyId>{id};
  }
  static bool bodyVisible(const MainWindow& window, BodyId id) {
    const auto* body = window.document_.findBody(id);
    return body && body->visible();
  }
  static bool transientToolsInactive(const MainWindow& window) {
    return window.partDesignCoordinator_.activeTool() == PartDesignToolKind::None &&
           !window.viewport_->toolPreviewShape_ &&
           !window.viewport_->toolCutPreviewShape_ &&
           window.viewport_->toolPreviewReplacedBodyIds_.empty() &&
           !window.viewport_->extrusionManipulatorVisible_ &&
           !window.viewport_->toolManipulator_ &&
           !window.viewport_->translationToolManipulator_ &&
           !window.viewport_->angularToolManipulator_ &&
           window.viewport_->pickMode_ == Viewport::PickMode::None &&
           window.viewport_->selectionFilter() == SelectionFilter::Any &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Fillet).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Chamfer).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::JoinBodies).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Shell).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Draft, window.document_).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Revolve, window.document_).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Mirror).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::LinearPattern).lifecycle == ToolLifecycle::Inactive &&
           window.partDesignCoordinator_.snapshot(PartDesignToolKind::CircularPattern).lifecycle == ToolLifecycle::Inactive &&
           window.previewUpdates_ && !window.previewUpdates_->hasPending() &&
           (!window.toolParametersDock_ || window.toolParametersDock_->isHidden()) &&
           (!window.revolveDock_ || window.revolveDock_->isHidden()) &&
           (!window.mirrorDock_ || window.mirrorDock_->isHidden()) &&
           (!window.linearPatternDock_ || window.linearPatternDock_->isHidden()) &&
           (!window.circularPatternDock_ || window.circularPatternDock_->isHidden());
  }
  static bool rejectedBeginLeavesNoToolUi(MainWindow& window,
                                           PartDesignToolKind kind) {
    window.resetTransientModelingUi();
    const auto preview = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(4.0, 4.0, 4.0).Shape());
    window.viewport_->setToolPreviewShape(1, 2, preview);
    window.viewport_->setEdgeMultiSelectionMode(true);
    window.viewport_->setFaceMultiSelectionMode(true);
    window.viewport_->setSelectionFilter(SelectionFilter::Edge);
    window.toolParametersDock_->show();

    PartDesignTransitionOutcome outcome;
    switch (kind) {
      case PartDesignToolKind::Fillet:
        outcome = window.partDesignCoordinator_.beginFillet(
            kInvalidBodyId, 2, {}, {}, 1.0);
        break;
      case PartDesignToolKind::Chamfer:
        outcome = window.partDesignCoordinator_.beginChamfer(
            1, kInvalidFeatureId, {}, {}, 1.0);
        break;
      case PartDesignToolKind::Shell:
        outcome = window.partDesignCoordinator_.beginShell(1, 2, {});
        break;
      case PartDesignToolKind::Draft:
        outcome = window.partDesignCoordinator_.beginDraft(
            window.document_, 1, 2, {});
        break;
      default:
        return false;
    }
    const bool activated = window.applyPartDesignBeginResult(outcome, kind);
    return !activated && outcome.effect == PartDesignTransitionEffect::Rejected &&
           !window.viewport_->edgeMultiSelectionMode() &&
           !window.viewport_->faceMultiSelectionMode() &&
           window.viewport_->selectedBodies().empty() &&
           window.viewport_->selectedBodyEdges().empty() &&
           window.viewport_->selectedBodyFaces().empty() &&
           transientToolsInactive(window);
  }
  static BodyId featureOwner(const MainWindow& window, FeatureId id) {
    const Body* body = window.document_.findBodyForFeature(id);
    return body ? body->id() : kInvalidBodyId;
  }
  static MainWindow::HistorySelectionState selectionState(
      const MainWindow& window) {
    return window.captureHistorySelection();
  }
  static bool exactSelectionState(
      const MainWindow::HistorySelectionState& left,
      const MainWindow::HistorySelectionState& right) {
    const bool same = left.bodies == right.bodies && left.edges == right.edges &&
           left.faces == right.faces &&
           left.historyPosition == right.historyPosition &&
           left.atEnd == right.atEnd &&
           left.historicalLegacyExtrusionSourceSketchId ==
               right.historicalLegacyExtrusionSourceSketchId &&
           left.presentation == right.presentation &&
           left.historyBodyId == right.historyBodyId &&
           left.historyFeatureId == right.historyFeatureId &&
           left.historySketchId == right.historySketchId;
    if (!same)
      std::cerr << "exact selection mismatch: b/e/f " << left.bodies.size()
                << '/' << right.bodies.size() << ' ' << left.edges.size()
                << '/' << right.edges.size() << ' ' << left.faces.size()
                << '/' << right.faces.size() << " pos "
                << left.historyPosition << '/' << right.historyPosition
                << " end " << left.atEnd << '/' << right.atEnd
                << " keys " << left.historyBodyId << ','
                << left.historyFeatureId << ',' << left.historySketchId
                << '/' << right.historyBodyId << ','
                << right.historyFeatureId << ',' << right.historySketchId
                << " legacy "
                << left.historicalLegacyExtrusionSourceSketchId.value_or(0)
                << '/'
                << right.historicalLegacyExtrusionSourceSketchId.value_or(0)
                << '\n';
    return same;
  }
  static void editHistoryFeature(MainWindow& window, BodyId bodyId,
                                 FeatureId featureId) {
    window.editHistoryFeature(bodyId, featureId);
  }
  static bool editTransactionActive(const MainWindow& window) {
    return window.editUiTransaction_.has_value();
  }
  static ToolLifecycle faceExtrudeLifecycle(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle;
  }
  static std::optional<FeatureId> faceExtrudeEditingFeature(
      const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).editingFeatureId;
  }
  static ToolLifecycle moveLifecycle(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle;
  }
  static std::optional<FeatureId> moveEditingFeature(
      const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).editingFeatureId;
  }
  static void cancelFaceExtrude(MainWindow& window) {
    window.cancelFaceExtrudeTool();
  }
  static void cancelMove(MainWindow& window) { window.cancelMoveTool(); }
  static int historyPosition(const MainWindow& window) {
    return window.historyPosition_;
  }
  static bool sameSelectionState(
      const MainWindow::HistorySelectionState& left,
      const MainWindow::HistorySelectionState& right) {
    const bool sameHistoryLocation = left.atEnd == right.atEnd &&
        (left.atEnd ||
         (left.historyBodyId == right.historyBodyId &&
          left.historyFeatureId == right.historyFeatureId &&
          left.historySketchId == right.historySketchId));
    const bool same = left.bodies == right.bodies && left.edges == right.edges &&
           left.faces == right.faces &&
           sameHistoryLocation;
    if (!same) {
      std::cerr << "selection mismatch: bodies " << left.bodies.size() << '/'
                << right.bodies.size() << " edges " << left.edges.size()
                << '/' << right.edges.size() << " faces " << left.faces.size()
                << '/' << right.faces.size() << " atEnd " << left.atEnd << '/'
                << right.atEnd << " pos " << left.historyPosition << '/'
                << right.historyPosition << " keys " << left.historyBodyId
                << ',' << left.historyFeatureId << ',' << left.historySketchId
                << '/' << right.historyBodyId << ',' << right.historyFeatureId
                << ',' << right.historySketchId << '\n';
    }
    return same;
  }
  static bool finishSketchEdit(MainWindow& window, SketchId sketchId,
                               const sketch::Sketch& geometry,
                               SketchPlacement placement) {
    const auto before = window.modelHistory_.undoCount();
    window.editSketchById(sketchId);
    if (!window.editingSketchIndex_) return false;
    window.sketchCanvas_->loadSketch(geometry);
    window.currentSketchPlacement_ = placement;
    window.finishSketch();
    return window.workspaceStack_->currentWidget() == window.viewport_ &&
           window.modelHistory_.undoCount() == before + 1;
  }
  static bool finishSketchEditNoCommand(MainWindow& window,
                                        SketchId sketchId,
                                        const sketch::Sketch& geometry,
                                        SketchPlacement placement) {
    const auto beforeUndo = window.modelHistory_.undoCount();
    const auto beforeRedo = window.modelHistory_.redoCount();
    window.editSketchById(sketchId);
    if (!window.editingSketchIndex_) return false;
    window.sketchCanvas_->loadSketch(geometry);
    window.currentSketchPlacement_ = placement;
    window.finishSketch();
    return window.workspaceStack_->currentWidget() == window.viewport_ &&
           window.modelHistory_.undoCount() == beforeUndo &&
           window.modelHistory_.redoCount() == beforeRedo;
  }
  static std::size_t redoCount(const MainWindow& window) {
    return window.modelHistory_.redoCount();
  }
  static QString statusMessage(const MainWindow& window) {
    return window.statusBar()->currentMessage();
  }
  static bool hasCommittedModernSolid(const MainWindow& window) {
    return window.hasCommittedModernSolid();
  }
  static bool hasDisplayableModernSolid(const MainWindow& window) {
    return window.hasDisplayableModernSolid();
  }
  static bool foreignShapeIsDisplayable(const MainWindow& window) {
    if (window.document_.bodies().empty()) return false;
    auto foreign = std::make_shared<const TopoDS_Shape>(
        BRepPrimAPI_MakeBox(3.0, 4.0, 5.0).Shape());
    return MainWindow::resolveModernSolidPresentation(
               window.document_.bodies().front(), foreign, true)
        .has_value();
  }
  static bool viewportHasBodyPresentations(const MainWindow& window) {
    return !window.viewport_->bodyViewShapes_.empty();
  }
  static bool hasExportableModernSolid(const MainWindow& window,
                                       QString* error = nullptr) {
    return window.hasExportableModernSolid(error);
  }
  static std::optional<SketchId> historicalLegacySourceSketchId(
      const MainWindow& window) {
    return window.historicalLegacyExtrusionSourceSketchId_;
  }
  static bool sketchConsumedByModernFeature(const MainWindow& window,
                                            SketchId sketchId) {
    return isSketchConsumedByPartDesign(window.document_, sketchId);
  }
  static QAction* undoAction(MainWindow& window) { return window.undoAction_; }
  static QAction* redoAction(MainWindow& window) { return window.redoAction_; }
  static QStackedWidget* workspace(MainWindow& window) {
    return window.workspaceStack_;
  }
  static SketchCanvas* sketch(MainWindow& window) { return window.sketchCanvas_; }
  static Viewport* viewport(MainWindow& window) { return window.viewport_; }
  static SketchPlacement currentSketchPlacement(const MainWindow& window) {
    return window.currentSketchPlacement_;
  }
  static bool selectedExtrusionIsLegacyFace(const MainWindow& window) {
    return window.selectedExtrusionSource_ &&
           std::holds_alternative<LegacySolidFacePick>(
               window.selectedExtrusionSource_->source);
  }
  static bool historicalLegacyExtrusionActive(const MainWindow& window) {
    return window.hasHistoricalLegacyExtrusion();
  }
  static bool legacySolidSketchEquals(const MainWindow& window,
                                      const sketch::Sketch& expected) {
    return window.viewport_->solidSketch_.semanticFingerprint() ==
               expected.semanticFingerprint() &&
           window.viewport_->solidSketch_.semanticallyEqual(expected);
  }
  static SketchId firstSketchId(const MainWindow& window) {
    return window.document_.sketches().empty()
               ? kInvalidSketchId
               : window.document_.sketches().front().id;
  }
  static bool commitLegacyExtrusionAsNewBody(MainWindow& window) {
    window.extrusionOperationCombo_->setCurrentIndex(
        static_cast<int>(ExtrudeOperation::NewBody));
    window.extrusionLengthSpin_->setValue(10.0);
    window.extrudeSketch();
    return !window.document_.bodies().empty();
  }
  static void removeBody(MainWindow& window, BodyId id) {
    window.removeBody(id);
  }
  static void removeHistoryFeature(MainWindow& window, BodyId bodyId,
                                   FeatureId featureId) {
    HistoryStep step;
    step.bodyId = bodyId;
    step.featureId = featureId;
    step.title = QStringLiteral("Stage 2 removal");
    window.removeHistoryStep(step);
  }
  static void removeHistorySketch(MainWindow& window, SketchId sketchId) {
    HistoryStep step;
    step.sketchId = sketchId;
    step.title = QStringLiteral("Historical source sketch");
    window.removeHistoryStep(step);
  }
  static std::size_t bodyCount(const MainWindow& window) {
    return window.document_.bodies().size();
  }
  static BodyId firstBodyId(const MainWindow& window) {
    return window.document_.bodies().empty()
        ? kInvalidBodyId : window.document_.bodies().front().id();
  }
  static std::size_t bodyFeatureCount(const MainWindow& window,
                                      BodyId bodyId) {
    const Body* body = window.document_.findBody(bodyId);
    return body ? body->features().size() : 0;
  }
  static bool hasSketch(const MainWindow& window, SketchId sketchId) {
    return window.document_.findSketch(sketchId) != nullptr;
  }
  static std::size_t sketchViewCount(const MainWindow& window) {
    return window.sketchViews_.size();
  }
  static std::size_t documentSketchCount(const MainWindow& window) {
    return window.document_.sketches().size();
  }
  static bool displayedSketchesMatchDocument(const MainWindow& window) {
    if (window.viewport_->displaySketches_.size() !=
        window.sketchViews_.size())
      return false;
    const auto samePoint = [](const auto& left, const auto& right) {
      return left.x == right.x && left.y == right.y && left.z == right.z;
    };
    for (std::size_t index = 0; index < window.sketchViews_.size(); ++index) {
      const auto& view = window.sketchViews_[index];
      const auto* model = window.document_.findSketch(view.documentSketchId);
      if (!model) return false;
      const auto& displayed = window.viewport_->displaySketches_[index];
      if (displayed.sketchId != model->id ||
          displayed.geometry.semanticFingerprint() !=
              model->geometry.semanticFingerprint() ||
          !displayed.geometry.semanticallyEqual(model->geometry) ||
          !samePoint(displayed.placement.origin, model->placement.origin) ||
          !samePoint(displayed.placement.xDirection,
                     model->placement.xDirection) ||
          !samePoint(displayed.placement.yDirection,
                     model->placement.yDirection))
        return false;
    }
    return true;
  }
  static void injectMissingSketchViewAndRefresh(MainWindow& window) {
    if (window.sketchViews_.empty()) return;
    const auto* model = window.document_.findSketch(
        window.sketchViews_.front().documentSketchId);
    if (!model) return;
    window.sketchCanvas_->loadSketch(model->geometry);
    window.sketchViews_.front().documentSketchId =
        std::numeric_limits<SketchId>::max();
    window.refreshBodyViewFromDocument();
    window.editingSketchIndex_ = 0;
    window.workspaceStack_->setCurrentWidget(window.sketchCanvas_);
    window.finishSketch();
  }
  static bool sketchEditActive(const MainWindow& window) {
    return window.editingSketchIndex_.has_value();
  }
  static std::size_t displayedSketchCount(const MainWindow& window) {
    return window.viewport_->displaySketches_.size();
  }
  static QString displayedSketchLabel(const MainWindow& window,
                                      std::size_t index) {
    return index < window.viewport_->displaySketches_.size()
               ? window.viewport_->displaySketches_[index].supportName
               : QString{};
  }
  static bool noSketchPickState(const MainWindow& window) {
    const auto invalid = static_cast<std::size_t>(-1);
    return window.viewport_->selectedExtrusionSketchIndex_ == invalid &&
           window.viewport_->hoveredExtrusionSketchIndex_ == invalid &&
           window.viewport_->revolveAxisSketchIndex_ == invalid &&
           window.viewport_->selectedExtrusionSketch_.lines().empty() &&
           window.viewport_->selectedExtrusionSketch_.circles().empty() &&
           window.viewport_->selectedExtrusionSketch_.arcs().empty() &&
           window.viewport_->hoveredExtrusionSketch_.lines().empty() &&
           window.viewport_->hoveredExtrusionSketch_.circles().empty() &&
           window.viewport_->hoveredExtrusionSketch_.arcs().empty() &&
           window.viewport_->selectedExtrusionRegionSketches_.empty();
  }
  static void setRevolveAxisSketchIndex(MainWindow& window,
                                        std::size_t index) {
    window.viewport_->revolveAxisSketchIndex_ = index;
  }
  static std::size_t revolveAxisSketchIndex(const MainWindow& window) {
    return window.viewport_->revolveAxisSketchIndex_;
  }
  static QPointF extrusionDirectionForLabel(
      MainWindow& window, std::size_t index, const QString& label,
      const SketchPlacement& placement) {
    if (index >= window.viewport_->displaySketches_.size()) return {};
    auto& displayed = window.viewport_->displaySketches_[index];
    displayed.supportName = label;
    displayed.placement = placement;
    window.viewport_->selectedExtrusionSketchIndex_ = index;
    window.viewport_->selectedExtrusionSupport_ = label;
    window.viewport_->selectedExtrusionPlacement_ = placement;
    window.viewport_->selectedLegacySolidFace_.reset();
    window.viewport_->selectedExtrusionReverse_ = false;
    return window.viewport_->extrusionScreenOffset(25.0);
  }
  static void finishNewSketch(MainWindow& window,
                              const sketch::Sketch& geometry,
                              const SketchPlacement& placement,
                              const QString& presentationLabel) {
    window.editingSketchIndex_.reset();
    window.currentSketchPlacement_ = placement;
    window.currentSketchFaceReference_.reset();
    window.currentSketchSupport_ = presentationLabel;
    window.sketchCanvas_->loadSketch(geometry);
    window.workspaceStack_->setCurrentWidget(window.sketchCanvas_);
    window.finishSketch();
  }
  static std::optional<FaceReference> currentSketchFaceReference(
      const MainWindow& window) {
    return window.currentSketchFaceReference_;
  }
  static void reverseSketchPresentationOrder(MainWindow& window) {
    std::reverse(window.sketchViews_.begin(), window.sketchViews_.end());
    window.syncSketchPresentationFromDocument();
    window.refreshBodyViewFromDocument();
  }
  static bool modified(const MainWindow& window) {
    return window.isWindowModified();
  }
  static void save(MainWindow& window) { window.saveProject(); }
  static void queueInvalidSourcePreview(MainWindow& window, int* rebuilds,
                                        int* publications) {
    window.schedulePreviewUpdate(
        kInvalidBodyId, kInvalidFeatureId,
        [rebuilds] { ++*rebuilds; },
        [publications] { ++*publications; });
  }
  static void dispatchPreviewCadence(MainWindow& window) {
    window.previewUpdates_->dispatchCadenceForTests();
  }
  static std::uint64_t automaticExtrudeDetectionCount(
      const MainWindow& window) {
    return window.automaticExtrudeDetectionCount_;
  }
  static void scheduleAutomaticExtrudeDetection(MainWindow& window) {
    window.extrudeOperationManuallyChanged_ = false;
    window.scheduleAutomaticExtrudeOperation();
  }
  static void cancelLegacyExtrusion(MainWindow& window) {
    window.cancelLegacyExtrusion();
  }
  static bool legacyExtrusionDockVisible(const MainWindow& window) {
    // The parent MainWindow is deliberately never shown in this offscreen
    // test; isVisible() would therefore be false even when the dock itself is
    // in its shown state.
    return window.extrusionDock_ && !window.extrusionDock_->isHidden();
  }
  static void exposeLegacyExtrusion(MainWindow& window) {
    window.extrusionDock_->show();
    window.selectedExtrusionSource_ = ExtrusionSourcePick{
        LegacySolidFacePick{LegacySolidFace::EndCap, SketchPlacement::xy(),
                            {0.0, 0.0, 1.0}, {}},
        QStringLiteral("pending surface")};
    window.viewport_->beginExtrusionSurfaceSelection();
  }
  static bool legacyExtrusionSelectionActive(const MainWindow& window) {
    return window.viewport_->pickMode_ == Viewport::PickMode::ExtrusionSurface;
  }
  static void applyImportedDocument(MainWindow& window, Document staged) {
    window.applyImportedDocument(std::move(staged),
                                 QStringLiteral("pending-preview-test.step"));
  }
  static void startSketchExtrude(MainWindow& window, std::size_t index) {
    if (index >= window.sketchViews_.size()) return;
    const auto& entry = window.sketchViews_[index];
    const auto* sketch = window.document_.findSketch(entry.documentSketchId);
    if (!sketch) return;
    window.createSketchExtrude(
        ExtrusionSourcePick{SketchRegionPick{sketch->id, sketch->placement,
                                             sketch->geometry},
                            QStringLiteral("test profile")});
  }
  static bool startTopFaceExtrude(MainWindow& window, double zMm) {
    Body* body = window.document_.activeBody();
    if (!body || !body->activeFeature() || !body->resultShape()) return false;
    const auto faceIndex = test::topPlanarFace(*body->resultShape(), zMm);
    if (!faceIndex) return false;
    window.createFaceExtrude(makeFaceReference(
        *body->resultShape(), body->id(), body->activeFeature()->id(),
        *faceIndex));
    return true;
  }
  static void dragDirectExtrude(MainWindow& window, double length) {
    emit window.viewport_->toolManipulatorValueChanged(length);
    window.previewUpdates_->dispatchCadenceForTests();
  }
  static bool pressDirectExtrudeKey(MainWindow& window, Qt::Key key) {
    window.toolParametersPanel_->focusParameterInput();
    QApplication::processEvents();
    QWidget* focus = QApplication::focusWidget();
    if (!focus) return false;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(focus, &press);
    QApplication::sendEvent(focus, &release);
    QApplication::processEvents();
    return true;
  }
  static void commitDirectExtrudeFromViewport(MainWindow& window) {
    emit window.viewport_->toolParameterCommitted();
    QApplication::processEvents();
  }
  static ToolLifecycle directExtrudeLifecycle(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).lifecycle;
  }
  static ExtrudeOperation directExtrudeOperation(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).extrudeOperation;
  }
  static bool directExtrudeReversed(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).reversed;
  }
  static ShapeFeature::ShapePtr directExtrudePreview(const MainWindow& window) {
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Extrude).previewShape;
  }
  static PartDesignToolKind activeTool(const MainWindow& window) {
    return window.partDesignCoordinator_.activeTool();
  }
  static std::size_t bodyFeatureCount(const MainWindow& window) {
    const Body* body = window.document_.activeBody();
    return body ? body->features().size() : 0;
  }
  static ShapeFeature::ShapePtr bodyShape(const MainWindow& window) {
    const Body* body = window.document_.activeBody();
    return body ? body->resultShape() : ShapeFeature::ShapePtr{};
  }
  static ShapeFeature::ShapePtr displayedBodyShape(const MainWindow& window) {
    return window.viewport_->bodyShape_;
  }
  static bool viewportSolidVisible(const MainWindow& window) {
    return window.viewport_->solidVisible_;
  }
  static bool setFirstBodyVisible(MainWindow& window, bool visible) {
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != 3) continue;
      item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      return true;
    }
    return false;
  }
  static bool setOriginVisible(MainWindow& window, bool visible) {
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != 1) continue;
      item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      return true;
    }
    return false;
  }
  static bool setBasePlaneVisible(MainWindow& window, int plane,
                                  bool visible) {
    if (plane < 0 || plane > 2) return false;
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != 10 + plane) continue;
      item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      return true;
    }
    return false;
  }
  static bool setSketchVisible(MainWindow& window, std::size_t index,
                               bool visible) {
    const int kind = 20 + static_cast<int>(index);
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != kind) continue;
      item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
      return true;
    }
    return false;
  }
  static std::optional<bool> treeVisibility(const MainWindow& window,
                                            int kind) {
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items)
      if (item->data(0, Qt::UserRole).toInt() == kind)
        return item->checkState(0) == Qt::Checked;
    return std::nullopt;
  }
  static bool presentationMatchesTree(const MainWindow& window) {
    if (treeVisibility(window, 1) !=
        std::optional<bool>{window.viewport_->originVisible()}) {
      std::cerr << "presentation/tree origin mismatch\n";
      return false;
    }
    for (int plane = 0; plane < 3; ++plane)
      if (treeVisibility(window, 10 + plane) !=
          std::optional<bool>{window.viewport_->basePlaneVisible(plane)}) {
        std::cerr << "presentation/tree plane mismatch " << plane << '\n';
        return false;
      }
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      const int kind = item->data(0, Qt::UserRole).toInt();
      if (kind < 20) continue;
      const auto index = static_cast<std::size_t>(kind - 20);
      if (item->checkState(0) !=
          (window.viewport_->sketchVisible(index) ? Qt::Checked
                                                   : Qt::Unchecked)) {
        std::cerr << "presentation/tree sketch mismatch " << index << '\n';
        return false;
      }
    }
    return true;
  }
  static bool originVisible(const MainWindow& window) {
    return window.viewport_->originVisible();
  }
  static bool basePlaneVisible(const MainWindow& window, int plane) {
    return window.viewport_->basePlaneVisible(plane);
  }
  static bool sketchVisible(const MainWindow& window, std::size_t index) {
    return window.viewport_->sketchVisible(index);
  }
  static void commitBodyPosition(MainWindow& window, QPointF position) {
    const QPointF previous = window.viewport_->bodyPosition();
    window.viewport_->setBodyPosition(position);
    window.viewport_->bodyMoveCommitted(previous, position);
  }
  static QPointF bodyPosition(const MainWindow& window) {
    return window.viewport_->bodyPosition();
  }
  static bool startMoveForActiveBody(MainWindow& window) {
    window.createMove();
    Body* body = window.document_.activeBody();
    if (!body || !body->activeFeature() || !body->resultShape()) return false;
    window.partDesignCoordinator_.setMoveBody(body->id(), body->activeFeature()->id(),
                                    body->resultShape());
    window.updateMoveToolPreview();
    return window.partDesignCoordinator_.snapshot(PartDesignToolKind::Move).lifecycle == ToolLifecycle::PreviewValid;
  }
  static bool enterMoveXAndAccept(MainWindow& window, const QString& text) {
    if (!window.moveDock_ || !window.moveXSpin_) return false;
    auto* editor = window.moveXSpin_->findChild<QLineEdit*>();
    if (!editor) return false;
    editor->setText(text);
    for (QShortcut* shortcut : window.moveDock_->findChildren<QShortcut*>()) {
      if (shortcut->key() != QKeySequence(Qt::Key_Return)) continue;
      return QMetaObject::invokeMethod(shortcut, "activated",
                                       Qt::DirectConnection);
    }
    return false;
  }
  static std::optional<Vector3d> lastMoveOffset(const MainWindow& window) {
    const Body* body = window.document_.activeBody();
    if (!body || body->features().empty()) return std::nullopt;
    const auto* move =
        dynamic_cast<const MoveFeature*>(body->features().back().get());
    return move ? std::optional<Vector3d>(move->offsetMm()) : std::nullopt;
  }
  static bool bodyRowsHaveNoFeatureChildren(const MainWindow& window) {
    bool foundBody = false;
    const auto items = window.featureTree_->findItems(
        QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
    for (QTreeWidgetItem* item : items) {
      if (item->data(0, Qt::UserRole).toInt() != 3) continue;
      foundBody = true;
      if (item->childCount() != 0) return false;
    }
    return foundBody;
  }
  static bool viewportHasToolPreview(const MainWindow& window) {
    return (window.viewport_->toolPreviewShape_ &&
            !window.viewport_->toolPreviewShape_->IsNull()) ||
           (window.viewport_->toolCutPreviewShape_ &&
            !window.viewport_->toolCutPreviewShape_->IsNull());
  }
  static std::size_t historyStepCount(const MainWindow& window) {
    return window.historySteps_.size();
  }
  static bool historyContainsFeature(const MainWindow& window,
                                     FeatureId id) {
    return std::any_of(window.historySteps_.begin(), window.historySteps_.end(),
                       [id](const HistoryStep& step) {
                         return step.featureId == id;
                       });
  }
  static void applyHistoryPosition(MainWindow& window, int position) {
    window.applyHistoryPosition(position);
  }
  static void moveHistoryToEnd(MainWindow& window) {
    window.moveHistoryToEnd();
    window.applyHistoryPosition(window.historyPosition_);
  }
  static bool historyAtEnd(const MainWindow& window) {
    return window.isHistoryAtEnd();
  }
  static void editSketch(MainWindow& window, SketchId sketchId) {
    window.editSketchById(sketchId);
  }
};

}  // namespace solidar

// Every critical check uses CHECK (not assert) so it remains active in the
// Release CI build where NDEBUG is defined.
namespace {

solidar::Document makeHeavyImportedDocument(int bodyCount) {
  solidar::Document document;
  for (int bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (int shapeIndex = 0; shapeIndex < 120; ++shapeIndex) {
      const double x = static_cast<double>(shapeIndex % 12) * 3.0;
      const double y = static_cast<double>(shapeIndex / 12) * 3.0;
      builder.Add(compound,
                  BRepPrimAPI_MakeBox(
                      gp_Pnt(x, y, static_cast<double>(bodyIndex) * 4.0),
                      2.0, 2.0, 2.0)
                      .Shape());
    }
    auto& body = document.addBody("CAD history body");
    body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
        std::make_shared<const TopoDS_Shape>(compound), "CAD compound"));
  }
  if (!document.recompute()) return {};
  return document;
}

struct HistoryMatrixFixture {
  std::string label;
  solidar::Document before;
  solidar::Document after;
  solidar::BodyId bodyId{solidar::kInvalidBodyId};
  solidar::BodyId createSelectionBodyId{solidar::kInvalidBodyId};
  solidar::FeatureId featureId{solidar::kInvalidFeatureId};
  solidar::BodyId sentinelBodyId{solidar::kInvalidBodyId};
  solidar::FeatureId sentinelFeatureId{solidar::kInvalidFeatureId};
  solidar::FeatureKind featureKind{solidar::FeatureKind::Unknown};
};

struct BoxSeed {
  solidar::BodyId bodyId{};
  solidar::SketchId sketchId{};
  solidar::FeatureId featureId{};
};

std::optional<BoxSeed> addBox(solidar::Document& document,
                              double x0 = 0.0, double x1 = 20.0,
                              double height = 10.0,
                              const std::string& name = "Box") {
  auto& sketch = document.addSketch(name + " profile");
  sketch.geometry.addRectangle({x0, 0.0}, {x1, 20.0});
  auto& body = document.addBody(name);
  auto feature = std::make_unique<solidar::ExtrudeFeature>(
      sketch.id, height, name + " Extrude");
  const auto featureId = feature->id();
  body.addFeature(std::move(feature));
  if (!document.recompute()) return std::nullopt;
  return BoxSeed{body.id(), sketch.id, featureId};
}

std::optional<HistoryMatrixFixture> makeHistoryMatrixFixture(
    const std::string& kind) {
  solidar::Document document;
  solidar::Document before;
  solidar::BodyId targetBody = solidar::kInvalidBodyId;
  solidar::BodyId createSelectionBody = solidar::kInvalidBodyId;
  solidar::FeatureId targetFeature = solidar::kInvalidFeatureId;
  const auto sentinel = addBox(document, 5.0, 15.0, 5.0,
                               "Selection sentinel");
  if (!sentinel) return std::nullopt;

  if (kind == "Extrude") {
    auto& sketch = document.addSketch("Extrude profile");
    sketch.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    before = document;
    auto& body = document.addBody("Extrude");
    auto feature = std::make_unique<solidar::ExtrudeFeature>(
        sketch.id, 20.0, "Extrude", solidar::ExtrudeOperation::NewBody,
        false);
    targetBody = body.id(); targetFeature = feature->id();
    body.addFeature(std::move(feature));
  } else if (kind == "Revolve") {
    auto& sketch = document.addSketch("Revolve profile");
    sketch.geometry.addRectangle({10.0, 5.0}, {30.0, 15.0});
    before = document;
    auto& body = document.addBody("Revolve body");
    auto feature = std::make_unique<solidar::RevolveFeature>(
        sketch.id,
        solidar::AxisReference{
            solidar::AxisReferenceType::SketchHorizontalAxis, sketch.id,
            solidar::sketch::kInvalidGeometryId},
        270.0, "Revolve", solidar::ExtrudeOperation::NewBody);
    targetBody = body.id(); targetFeature = feature->id();
    body.addFeature(std::move(feature));
  } else if (kind == "Join") {
    const auto first = addBox(document, 0.0, 20.0, 10.0, "Join A");
    const auto second = addBox(document, 10.0, 30.0, 10.0, "Join B");
    const auto replacement = addBox(document, 5.0, 25.0, 10.0, "Join C");
    if (!first || !second || !replacement) return std::nullopt;
    before = document;
    createSelectionBody = first->bodyId;
    auto& body = document.addBody("Join result");
    auto feature = std::make_unique<solidar::JoinBodiesFeature>(
        first->bodyId, first->featureId, second->bodyId, second->featureId,
        "Join");
    targetBody = body.id(); targetFeature = feature->id();
    body.addFeature(std::move(feature));
    document.findBody(first->bodyId)->setVisible(false);
    document.findBody(second->bodyId)->setVisible(false);
  } else {
    const double x0 = (kind == "Mirror" || kind == "CircularPattern")
                          ? 10.0 : 0.0;
    const auto base = addBox(document, x0, x0 + 20.0, 20.0, kind);
    if (!base) return std::nullopt;
    auto* body = document.findBody(base->bodyId);
    if (!body) return std::nullopt;
    targetBody = body->id();
    createSelectionBody = body->id();
    if (kind == "FaceExtrude") {
      const auto shape = body->resultShape();
      const auto face = shape ? solidar::test::topPlanarFace(*shape, 20.0)
                              : std::nullopt;
      if (!shape || !face) return std::nullopt;
      auto feature = std::make_unique<solidar::ExtrudeFeature>(
          solidar::makeFaceReference(*shape, body->id(), base->featureId,
                                     *face),
          5.0, "Face Extrude", solidar::ExtrudeOperation::Join, false);
      before = document;
      targetFeature = feature->id();
      body->addFeature(std::move(feature));
    } else if (kind == "Move") {
      auto feature = std::make_unique<solidar::MoveFeature>(
          base->featureId, solidar::Vector3d{5.0, 0.0, 0.0}, "Move");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Mirror") {
      auto feature = std::make_unique<solidar::MirrorFeature>(
          base->featureId, solidar::MirrorPlane::YZ, "Mirror");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "LinearPattern") {
      auto feature = std::make_unique<solidar::LinearPatternFeature>(
          base->bodyId, base->featureId, solidar::PrincipalAxis::X, 3, 40.0,
          solidar::PatternOperation::Join, "Linear Pattern");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "CircularPattern") {
      auto feature = std::make_unique<solidar::CircularPatternFeature>(
          base->bodyId, base->featureId, solidar::PrincipalAxis::Z, 4, 360.0,
          solidar::PatternOperation::Join, "Circular Pattern");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "LinearPatternNewBody") {
      before = document;
      auto& result = document.addBody("Linear Pattern NewBody");
      auto feature = std::make_unique<solidar::LinearPatternFeature>(
          base->bodyId, base->featureId, solidar::PrincipalAxis::X, 3, 40.0,
          solidar::PatternOperation::NewBody, "Linear Pattern NewBody");
      targetBody = result.id(); targetFeature = feature->id();
      result.addFeature(std::move(feature));
    } else if (kind == "CircularPatternNewBody") {
      before = document;
      auto& result = document.addBody("Circular Pattern NewBody");
      auto feature = std::make_unique<solidar::CircularPatternFeature>(
          base->bodyId, base->featureId, solidar::PrincipalAxis::Z, 4, 360.0,
          solidar::PatternOperation::NewBody, "Circular Pattern NewBody");
      targetBody = result.id(); targetFeature = feature->id();
      result.addFeature(std::move(feature));
    } else if (kind == "Fillet") {
      const auto shape = body->resultShape();
      if (!shape) return std::nullopt;
      auto feature = std::make_unique<solidar::FilletFeature>(
          solidar::makeEdgeReference(*shape, body->id(), base->featureId, 0),
          2.0, "Fillet");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Chamfer") {
      const auto shape = body->resultShape();
      if (!shape) return std::nullopt;
      auto feature = std::make_unique<solidar::ChamferFeature>(
          solidar::makeEdgeReference(*shape, body->id(), base->featureId, 0),
          1.0, "Chamfer");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Shell") {
      const auto shape = body->resultShape();
      const auto face = shape ? solidar::test::topPlanarFace(*shape, 20.0)
                              : std::nullopt;
      if (!face) return std::nullopt;
      auto feature = std::make_unique<solidar::ShellFeature>(
          base->featureId,
          std::vector{solidar::makeFaceReference(
              *shape, body->id(), base->featureId, *face)},
          2.0, false, "Shell");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Draft") {
      const auto shape = body->resultShape();
      if (!shape) return std::nullopt;
      std::optional<std::size_t> side;
      std::size_t index = 0;
      for (TopExp_Explorer faces(*shape, TopAbs_FACE); faces.More();
           faces.Next(), ++index) {
        const auto placement = solidar::resolveFacePlacement(*shape, index);
        if (placement.planar &&
            std::abs(placement.placement.normal().z) < 0.1) {
          side = index; break;
        }
      }
      if (!side) return std::nullopt;
      auto feature = std::make_unique<solidar::DraftFeature>(
          base->featureId,
          std::vector{solidar::makeFaceReference(
              *shape, body->id(), base->featureId, *side)},
          solidar::PlaneReference{solidar::NeutralPlaneType::GlobalXY},
          solidar::AxisReference{solidar::AxisReferenceType::GlobalZ,
                                 solidar::kInvalidSketchId,
                                 solidar::sketch::kInvalidGeometryId},
          5.0, false, "Draft");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else if (kind == "Pocket") {
      const auto shape = body->resultShape();
      const auto face = shape ? solidar::test::topPlanarFace(*shape, 20.0)
                              : std::nullopt;
      if (!face) return std::nullopt;
      auto& profile = document.addSketch("Pocket profile");
      if (!document.attachSketchToFace(
              profile.id, solidar::makeFaceReference(
                  *shape, body->id(), base->featureId, *face)))
        return std::nullopt;
      const auto first = profile.placement.toLocal({5.0, 5.0, 20.0});
      const auto second = profile.placement.toLocal({10.0, 10.0, 20.0});
      profile.geometry.addRectangle({first.x, first.y},
                                    {second.x, second.y});
      if (!document.recompute()) return std::nullopt;
      auto feature = std::make_unique<solidar::PocketFeature>(
          profile.id, 5.0, "Pocket");
      before = document;
      targetFeature = feature->id(); body->addFeature(std::move(feature));
    } else {
      return std::nullopt;
    }
  }

  if (!document.recompute()) return std::nullopt;
  const auto* feature = document.findFeature(targetFeature);
  if (!feature || !feature->isValid()) return std::nullopt;
  const auto featureKind = feature->kind();
  return HistoryMatrixFixture{kind, std::move(before), std::move(document),
                              targetBody, createSelectionBody, targetFeature,
                              sentinel->bodyId, sentinel->featureId,
                               featureKind};
}

std::optional<solidar::Document> makeExpectedEditedDocument(
    const HistoryMatrixFixture& fixture) {
  solidar::Document expected = fixture.after;
  auto* body = expected.findBody(fixture.bodyId);
  auto* feature = expected.findFeature(fixture.featureId);
  if (!body || !feature) return std::nullopt;
  const auto index = body->featureIndex(fixture.featureId);
  if (!index) return std::nullopt;

  if (auto* item = dynamic_cast<solidar::ExtrudeFeature*>(feature)) {
    item->setLengthMm(25.0);
  } else if (auto* item = dynamic_cast<solidar::RevolveFeature*>(feature)) {
    item->setAngleDeg(180.0);
  } else if (auto* item = dynamic_cast<solidar::PocketFeature*>(feature)) {
    item->setDepthMm(7.0);
  } else if (auto* item = dynamic_cast<solidar::FilletFeature*>(feature)) {
    item->setRadiusMm(1.0);
  } else if (auto* item = dynamic_cast<solidar::ChamferFeature*>(feature)) {
    item->setDistanceMm(0.5);
  } else if (auto* item = dynamic_cast<solidar::ShellFeature*>(feature)) {
    item->setThicknessMm(1.0);
  } else if (auto* item = dynamic_cast<solidar::DraftFeature*>(feature)) {
    item->setAngleDeg(3.0);
  } else if (auto* item = dynamic_cast<solidar::MoveFeature*>(feature)) {
    item->setOffsetMm({7.0, 1.0, 0.0});
  } else if (auto* item = dynamic_cast<solidar::MirrorFeature*>(feature)) {
    item->setPlane(solidar::MirrorPlane::XZ);
  } else if (auto* item =
                 dynamic_cast<solidar::LinearPatternFeature*>(feature)) {
    item->setCount(2);
    item->setSpacingMm(50.0);
  } else if (auto* item =
                 dynamic_cast<solidar::CircularPatternFeature*>(feature)) {
    item->setCount(3);
    item->setAngleDeg(270.0);
  } else if (auto* item =
                 dynamic_cast<solidar::JoinBodiesFeature*>(feature)) {
    const solidar::Body* replacement = nullptr;
    for (const auto& candidate : expected.bodies()) {
      if (candidate.id() == fixture.bodyId ||
          candidate.id() == item->firstBodyId() ||
          candidate.id() == item->secondBodyId() || !candidate.activeFeature())
        continue;
      replacement = &candidate;
      break;
    }
    if (!replacement || !replacement->activeFeature()) return std::nullopt;
    const auto oldSecond = item->secondBodyId();
    item->setInputs(item->firstBodyId(), item->firstFeatureId(),
                    replacement->id(), replacement->activeFeature()->id());
    if (auto* detached = expected.findBody(oldSecond)) detached->setVisible(true);
    if (auto* attached = expected.findBody(replacement->id()))
      attached->setVisible(false);
  } else {
    return std::nullopt;
  }
  body->markDirtyFrom(*index);
  if (!expected.recompute()) return std::nullopt;
  return expected;
}

template <typename Reference>
void appendTopologyReference(std::ostringstream& out,
                             const Reference& reference) {
  out << reference.bodyId << ',' << reference.featureId << ',';
  if constexpr (std::is_same_v<Reference, solidar::FaceReference>)
    out << reference.faceIndex;
  else
    out << reference.edgeIndex;
  out << ',' << reference.persistentTag << ',';
  if (reference.signature) {
    const auto& signature = *reference.signature;
    if constexpr (std::is_same_v<Reference, solidar::FaceReference>) {
      out << static_cast<int>(signature.surface) << ',' << signature.area << ','
          << signature.centroid.x << ',' << signature.centroid.y << ','
          << signature.centroid.z << ',' << signature.normal.x << ','
          << signature.normal.y << ',' << signature.normal.z << ','
          << signature.bounds.minimum.x << ',' << signature.bounds.minimum.y
          << ',' << signature.bounds.minimum.z << ','
          << signature.bounds.maximum.x << ',' << signature.bounds.maximum.y
          << ',' << signature.bounds.maximum.z << ',' << signature.radius
          << ',' << signature.axis.x << ',' << signature.axis.y << ','
          << signature.axis.z;
    } else {
      out << static_cast<int>(signature.curve) << ',' << signature.length << ','
          << signature.midpoint.x << ',' << signature.midpoint.y << ','
          << signature.midpoint.z << ',' << signature.tangent.x << ','
          << signature.tangent.y << ',' << signature.tangent.z << ','
          << signature.bounds.minimum.x << ',' << signature.bounds.minimum.y
          << ',' << signature.bounds.minimum.z << ','
          << signature.bounds.maximum.x << ',' << signature.bounds.maximum.y
          << ',' << signature.bounds.maximum.z << ',' << signature.radius
          << ',' << signature.center.x << ',' << signature.center.y << ','
          << signature.center.z;
    }
  } else {
    out << "none";
  }
}

std::string historyMatrixPayload(const solidar::ShapeFeature& feature,
                                 bool includeIdentity = true) {
  std::ostringstream out;
  out.precision(17);
  out << static_cast<int>(feature.kind());
  if (includeIdentity) out << ':' << feature.id() << ':' << feature.name();
  if (const auto* item = dynamic_cast<const solidar::ExtrudeFeature*>(&feature)) {
    out << ":face=" << item->isFaceSource();
    if (item->isFaceSource()) {
      if (item->faceReference()) appendTopologyReference(out, *item->faceReference());
    } else {
      out << ":sketch=" << item->profileSketchId();
    }
    out << ":override=" << item->profileOverride().has_value();
    if (item->profileOverride())
      out << ':' << item->profileOverride()->semanticFingerprint();
    out << ':' << item->lengthMm() << ':' << static_cast<int>(item->operation())
        << ':' << item->reversed();
  } else if (const auto* item = dynamic_cast<const solidar::RevolveFeature*>(&feature)) {
    out << ':' << item->profileSketchId() << ':'
        << static_cast<int>(item->axis().type) << ':' << item->axis().sketchId
        << ':' << item->axis().lineId << ':' << item->angleDeg() << ':'
        << static_cast<int>(item->operation()) << ':' << item->reversed()
        << ':' << item->profileOverride().has_value();
    if (item->profileOverride())
      out << ':' << item->profileOverride()->semanticFingerprint();
  } else if (const auto* item = dynamic_cast<const solidar::PocketFeature*>(&feature)) {
    out << ':' << item->profileSketchId() << ':' << item->depthMm();
  } else if (const auto* item = dynamic_cast<const solidar::FilletFeature*>(&feature)) {
    out << ':' << item->radiusMm() << ':' << item->edges().size();
    for (const auto& edge : item->edges()) appendTopologyReference(out, edge);
  } else if (const auto* item = dynamic_cast<const solidar::ChamferFeature*>(&feature)) {
    out << ':' << item->distanceMm() << ':' << item->edges().size();
    for (const auto& edge : item->edges()) appendTopologyReference(out, edge);
  } else if (const auto* item = dynamic_cast<const solidar::ShellFeature*>(&feature)) {
    out << ':' << item->sourceFeatureId() << ':' << item->thicknessMm() << ':'
        << item->outside() << ':' << item->removedFaces().size();
    for (const auto& face : item->removedFaces()) appendTopologyReference(out, face);
  } else if (const auto* item = dynamic_cast<const solidar::DraftFeature*>(&feature)) {
    out << ':' << item->sourceFeatureId() << ':' << item->angleDeg() << ':'
        << item->reversed() << ':' << static_cast<int>(item->neutralPlane().type)
        << ':' << static_cast<int>(item->pullDirection().type) << ':'
        << item->pullDirection().sketchId << ':' << item->pullDirection().lineId
        << ':' << item->draftedFaces().size();
    if (item->neutralPlane().face)
      appendTopologyReference(out, *item->neutralPlane().face);
    if (item->rotationEdge()) appendTopologyReference(out, *item->rotationEdge());
    for (const auto& face : item->draftedFaces()) appendTopologyReference(out, face);
  } else if (const auto* item = dynamic_cast<const solidar::MirrorFeature*>(&feature)) {
    out << ':' << item->sourceFeatureId() << ':' << static_cast<int>(item->plane());
  } else if (const auto* item = dynamic_cast<const solidar::MoveFeature*>(&feature)) {
    const auto offset = item->offsetMm();
    out << ':' << item->sourceFeatureId() << ':' << offset.x << ':' << offset.y
        << ':' << offset.z;
  } else if (const auto* item =
                 dynamic_cast<const solidar::LinearPatternFeature*>(&feature)) {
    out << ':' << item->sourceBodyId() << ':' << item->sourceFeatureId() << ':'
        << static_cast<int>(item->direction()) << ':' << item->count() << ':'
        << item->spacingMm() << ':' << static_cast<int>(item->operation());
  } else if (const auto* item =
                 dynamic_cast<const solidar::CircularPatternFeature*>(&feature)) {
    out << ':' << item->sourceBodyId() << ':' << item->sourceFeatureId() << ':'
        << static_cast<int>(item->axis()) << ':' << item->count() << ':'
        << item->angleDeg() << ':' << static_cast<int>(item->operation());
  } else if (const auto* item = dynamic_cast<const solidar::JoinBodiesFeature*>(&feature)) {
    out << ':' << item->firstBodyId() << ':' << item->firstFeatureId() << ':'
        << item->secondBodyId() << ':' << item->secondFeatureId();
  }
  return out.str();
}

struct MatrixShapeSignature {
  std::size_t solids{};
  std::size_t faces{};
  std::size_t edges{};
  std::size_t rawFaces{};
  std::size_t rawEdges{};
  double volume{};
  solidar::test::Bounds bounds;
};

std::optional<MatrixShapeSignature> matrixShapeSignature(
    const solidar::ShapeFeature* feature) {
  if (!feature || !feature->isValid() || !feature->error().empty() ||
      !feature->lastValidShape() || feature->lastValidShape()->IsNull() ||
      !BRepCheck_Analyzer(*feature->lastValidShape()).IsValid())
    return std::nullopt;
  std::string error;
  const auto topology = feature->lastValidTopologyIndex(&error);
  if (!topology || !error.empty()) return std::nullopt;
  return MatrixShapeSignature{
      solidar::test::solidCount(*feature->lastValidShape()),
      topology->faceCount(), topology->edgeCount(), topology->rawFaceCount(),
      topology->rawEdgeCount(),
      solidar::test::volumeOf(*feature->lastValidShape()),
      solidar::test::boundsOf(*feature->lastValidShape())};
}

bool sameMatrixShape(const MatrixShapeSignature& left,
                     const MatrixShapeSignature& right) {
  const auto near = [](double a, double b) {
    return solidar::test::near(a, b, 1e-5);
  };
  return left.solids == right.solids && left.faces == right.faces &&
         left.edges == right.edges && left.rawFaces == right.rawFaces &&
         left.rawEdges == right.rawEdges && near(left.volume, right.volume) &&
         near(left.bounds.minX, right.bounds.minX) &&
         near(left.bounds.minY, right.bounds.minY) &&
         near(left.bounds.minZ, right.bounds.minZ) &&
         near(left.bounds.maxX, right.bounds.maxX) &&
         near(left.bounds.maxY, right.bounds.maxY) &&
         near(left.bounds.maxZ, right.bounds.maxZ);
}

bool featureBelongsToBody(const solidar::Document& document,
                          solidar::BodyId bodyId,
                          solidar::FeatureId featureId) {
  const auto* body = document.findBody(bodyId);
  return body && std::any_of(
      body->features().begin(), body->features().end(),
      [featureId](const auto& feature) {
        return feature && feature->id() == featureId;
      });
}

bool faceReferenceResolves(const solidar::Document& document,
                           const solidar::FaceReference& reference) {
  if (!featureBelongsToBody(document, reference.bodyId,
                            reference.featureId))
    return false;
  const auto* feature = dynamic_cast<const solidar::ShapeFeature*>(
      document.findFeature(reference.featureId));
  const auto topology = feature ? feature->lastValidTopologyIndex() : nullptr;
  return topology && topology->resolveFace(reference.topology());
}

bool edgeReferenceResolves(const solidar::Document& document,
                           const solidar::EdgeReference& reference) {
  if (!featureBelongsToBody(document, reference.bodyId,
                            reference.featureId))
    return false;
  const auto* feature = dynamic_cast<const solidar::ShapeFeature*>(
      document.findFeature(reference.featureId));
  const auto topology = feature ? feature->lastValidTopologyIndex() : nullptr;
  return topology && topology->resolveEdge(reference.topology());
}

bool axisReferenceResolves(const solidar::Document& document,
                           const solidar::AxisReference& axis) {
  if (!solidar::isKnownAxisReferenceType(axis.type)) return false;
  if (!solidar::isSketchAxisReferenceType(axis.type)) return true;
  const auto* sketch = document.findSketch(axis.sketchId);
  if (!sketch) return false;
  return axis.type != solidar::AxisReferenceType::SketchLine ||
         sketch->geometry.lineIndex(axis.lineId).has_value();
}

bool matrixReferencesResolve(const solidar::Document& document,
                             const solidar::ShapeFeature& feature) {
  if (const auto* item =
          dynamic_cast<const solidar::ExtrudeFeature*>(&feature)) {
    return item->isFaceSource()
        ? item->faceReference() &&
              faceReferenceResolves(document, *item->faceReference())
        : document.findSketch(item->profileSketchId()) != nullptr;
  }
  if (const auto* item =
          dynamic_cast<const solidar::RevolveFeature*>(&feature))
    return document.findSketch(item->profileSketchId()) &&
           axisReferenceResolves(document, item->axis());
  if (const auto* item = dynamic_cast<const solidar::PocketFeature*>(&feature))
    return document.findSketch(item->profileSketchId()) != nullptr;
  if (const auto* item = dynamic_cast<const solidar::FilletFeature*>(&feature))
    return !item->edges().empty() &&
           std::all_of(item->edges().begin(), item->edges().end(),
                       [&document](const auto& edge) {
                         return edgeReferenceResolves(document, edge);
                       });
  if (const auto* item = dynamic_cast<const solidar::ChamferFeature*>(&feature))
    return !item->edges().empty() &&
           std::all_of(item->edges().begin(), item->edges().end(),
                       [&document](const auto& edge) {
                         return edgeReferenceResolves(document, edge);
                       });
  if (const auto* item = dynamic_cast<const solidar::ShellFeature*>(&feature))
    return document.findFeature(item->sourceFeatureId()) &&
           !item->removedFaces().empty() &&
           std::all_of(item->removedFaces().begin(), item->removedFaces().end(),
                       [&document](const auto& face) {
                         return faceReferenceResolves(document, face);
                       });
  if (const auto* item = dynamic_cast<const solidar::DraftFeature*>(&feature)) {
    if (!document.findFeature(item->sourceFeatureId()) ||
        !axisReferenceResolves(document, item->pullDirection()) ||
        item->draftedFaces().empty() ||
        !std::all_of(item->draftedFaces().begin(), item->draftedFaces().end(),
                     [&document](const auto& face) {
                       return faceReferenceResolves(document, face);
                     }))
      return false;
    if (item->neutralPlane().face &&
        !faceReferenceResolves(document, *item->neutralPlane().face))
      return false;
    return !item->rotationEdge() ||
           edgeReferenceResolves(document, *item->rotationEdge());
  }
  if (const auto* item = dynamic_cast<const solidar::JoinBodiesFeature*>(&feature))
    return featureBelongsToBody(document, item->firstBodyId(),
                                item->firstFeatureId()) &&
           featureBelongsToBody(document, item->secondBodyId(),
                                item->secondFeatureId());
  if (const auto* item = dynamic_cast<const solidar::MoveFeature*>(&feature))
    return document.findFeature(item->sourceFeatureId()) != nullptr;
  if (const auto* item = dynamic_cast<const solidar::MirrorFeature*>(&feature))
    return document.findFeature(item->sourceFeatureId()) != nullptr;
  if (const auto* item =
          dynamic_cast<const solidar::LinearPatternFeature*>(&feature))
    return featureBelongsToBody(document, item->sourceBodyId(),
                                item->sourceFeatureId());
  if (const auto* item =
          dynamic_cast<const solidar::CircularPatternFeature*>(&feature))
    return featureBelongsToBody(document, item->sourceBodyId(),
                                item->sourceFeatureId());
  return false;
}

std::optional<QByteArray> serializedDocumentDefinition(
    const solidar::Document& document) {
  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("unchanged-edit-document-XXXXXX")));
  if (!directory.isValid()) return std::nullopt;
  const QString path = directory.filePath(QStringLiteral("state.solidar"));
  QString error;
  if (!solidar::project::ProjectFile::saveDocument(path, document, &error))
    return std::nullopt;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
  QJsonParseError parseError;
  auto serialized = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !serialized.isObject())
    return std::nullopt;
  // createdAt is export metadata generated by saveDocument(), not persisted
  // Document state.  Canonicalize only that clock-dependent field; equality
  // below remains byte-exact for the complete model definition.
  auto root = serialized.object();
  root.remove(QStringLiteral("createdAt"));
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<QString> firstJsonDifference(const QJsonValue& left,
                                           const QJsonValue& right,
                                           const QString& path) {
  if (left.type() != right.type())
    return QStringLiteral("%1: type %2 != %3")
        .arg(path).arg(static_cast<int>(left.type()))
        .arg(static_cast<int>(right.type()));
  if (left.isObject()) {
    const auto leftObject = left.toObject();
    const auto rightObject = right.toObject();
    QStringList keys = leftObject.keys();
    for (const auto& key : rightObject.keys())
      if (!keys.contains(key)) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
      if (!leftObject.contains(key) || !rightObject.contains(key))
        return QStringLiteral("%1.%2: key presence %3 != %4")
            .arg(path, key)
            .arg(leftObject.contains(key)).arg(rightObject.contains(key));
      if (auto difference = firstJsonDifference(
              leftObject.value(key), rightObject.value(key),
              path + QLatin1Char('.') + key))
        return difference;
    }
    return std::nullopt;
  }
  if (left.isArray()) {
    const auto leftArray = left.toArray();
    const auto rightArray = right.toArray();
    if (leftArray.size() != rightArray.size())
      return QStringLiteral("%1: array size %2 != %3")
          .arg(path).arg(leftArray.size()).arg(rightArray.size());
    for (qsizetype index = 0; index < leftArray.size(); ++index) {
      if (auto difference = firstJsonDifference(
              leftArray.at(index), rightArray.at(index),
              QStringLiteral("%1[%2]").arg(path).arg(index)))
        return difference;
    }
    return std::nullopt;
  }
  if (left != right)
    return QStringLiteral("%1: '%2' != '%3'")
        .arg(path, left.toVariant().toString(), right.toVariant().toString());
  return std::nullopt;
}

QString firstSerializedDocumentDifference(const QByteArray& left,
                                          const QByteArray& right) {
  QJsonParseError leftError;
  QJsonParseError rightError;
  const auto leftDocument = QJsonDocument::fromJson(left, &leftError);
  const auto rightDocument = QJsonDocument::fromJson(right, &rightError);
  if (leftError.error != QJsonParseError::NoError ||
      rightError.error != QJsonParseError::NoError)
    return QStringLiteral("JSON parse errors: %1 / %2")
        .arg(leftError.errorString(), rightError.errorString());
  return firstJsonDifference(QJsonValue(leftDocument.object()),
                             QJsonValue(rightDocument.object()),
                             QStringLiteral("root"))
      .value_or(QStringLiteral("byte-only difference"));
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);

  QTemporaryDir directory(QDir::current().filePath(
      QStringLiteral("project-switch-ui-tests-XXXXXX")));
  CHECK(directory.isValid());

  // Project A carries a real body so the replacement tears down actual B-Rep
  // state, not an empty shell.
  {
    solidar::Document document;
    auto& baseSketch = document.addSketch("Base");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    auto& body = document.addBody("Body");
    body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 15.0, "Extrude"));
    CHECK(document.recompute());
    QString error;
    CHECK(solidar::project::ProjectFile::saveDocument(
        directory.filePath(QStringLiteral("a.solidar")), document, &error));
  }

  QString error;
  const QString pathA = directory.filePath(QStringLiteral("a.solidar"));
  const QString pathB = directory.filePath(QStringLiteral("b.solidar"));
  CHECK(solidar::project::ProjectFile::create(pathB, &error));
  solidar::AppSettings settings(
      directory.filePath(QStringLiteral("settings.ini")));
  using Access = solidar::MainWindowUndoTestAccess;

  // Historical v1 projects have a rendered legacy solid but no Body/B-Rep or
  // FaceReference. Its faces still publish a fully typed semantic source: the
  // presentation label is not used to recover placement or source kind.
  {
    const QString legacyPath =
        directory.filePath(QStringLiteral("historical-v1-solid.solidar"));
    solidar::project::ProjectData legacy;
    legacy.box = {60.0, 40.0, 25.0};
    solidar::sketch::Sketch profile;
    profile.addRectangle({-15.0, -10.0}, {15.0, 10.0});
    solidar::sketch::Sketch decoyProfile;
    decoyProfile.addRectangle({100.0, 100.0}, {105.0, 105.0});
    legacy.sketches.push_back(
        {decoyProfile, QStringLiteral("XY")});
    legacy.sketches.push_back(
        {profile, QStringLiteral("arbitrary presentation label")});
    legacy.hasExtrusion = true;
    legacy.extrusionSourceSketch = 1;
    CHECK(solidar::project::ProjectFile::save(legacyPath, legacy, &error));
    const QString missingSourcePath = directory.filePath(
        QStringLiteral("historical-v1-missing-source.solidar"));
    auto missingSource = legacy;
    missingSource.extrusionSourceSketch.reset();
    CHECK(solidar::project::ProjectFile::save(missingSourcePath,
                                              missingSource, &error));

    const auto click = [](solidar::Viewport& viewport, QPointF position) {
      QMouseEvent event(QEvent::MouseButtonPress, position, position,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(&viewport, &event);
    };

    solidar::MainWindow sketchEditor(settings);
    CHECK(sketchEditor.loadProject(legacyPath, &error));
    CHECK(Access::historicalLegacyExtrusionActive(sketchEditor));
    const auto stableLegacySource =
        Access::historicalLegacySourceSketchId(sketchEditor);
    CHECK(stableLegacySource);
    CHECK(!Access::hasCommittedModernSolid(sketchEditor));
    CHECK(!Access::hasDisplayableModernSolid(sketchEditor));
    QString legacyExportError;
    CHECK(!Access::hasExportableModernSolid(sketchEditor,
                                             &legacyExportError));
    CHECK(!legacyExportError.isEmpty());
    CHECK(Access::viewportSolidVisible(sketchEditor));
    // The v1 positional index is consumed only at the load boundary. Reordering
    // the derived UI rows cannot retarget the historical solid to another
    // sketch; stable ID resolution reconstructs the exact same profile.
    Access::reverseSketchPresentationOrder(sketchEditor);
    CHECK(Access::historicalLegacySourceSketchId(sketchEditor) ==
          stableLegacySource);
    CHECK(Access::legacySolidSketchEquals(sketchEditor, profile));
    Access::reverseSketchPresentationOrder(sketchEditor);
    CHECK(Access::historicalLegacySourceSketchId(sketchEditor) ==
          stableLegacySource);
    // Reapplying the end marker must not erase the presentation-only v1
    // prism merely because its Document intentionally contains no Body.
    Access::moveHistoryToEnd(sketchEditor);
    Access::moveHistoryToEnd(sketchEditor);
    CHECK(Access::viewportSolidVisible(sketchEditor));
    auto* sketchViewport = Access::viewport(sketchEditor);
    sketchViewport->resize(800, 600);
    sketchViewport->viewBottom();
    const QPointF endCap(sketchViewport->width() * 0.5,
                         sketchViewport->height() * 0.52);
    int sketchFaceEvents = 0;
    QObject::connect(
        sketchViewport, &solidar::Viewport::sketchPlanePicked, sketchViewport,
        [&](const solidar::SketchPlanePick& pick) {
          if (std::holds_alternative<solidar::LegacySolidFacePick>(pick.source))
            ++sketchFaceEvents;
        });
    sketchViewport->beginSketchPlaneSelection();
    click(*sketchViewport, endCap);
    CHECK(sketchFaceEvents == 1);
    CHECK(Access::workspace(sketchEditor)->currentWidget() ==
          Access::sketch(sketchEditor));
    CHECK(std::abs(Access::currentSketchPlacement(sketchEditor).origin.z -
                   25.0) < 1e-9);
    CHECK(Access::historicalLegacyExtrusionActive(sketchEditor));

    solidar::MainWindow extrusionEditor(settings);
    CHECK(extrusionEditor.loadProject(legacyPath, &error));
    CHECK(Access::historicalLegacyExtrusionActive(extrusionEditor));
    Access::moveHistoryToEnd(extrusionEditor);
    CHECK(Access::viewportSolidVisible(extrusionEditor));
    auto* extrusionViewport = Access::viewport(extrusionEditor);
    extrusionViewport->resize(800, 600);
    const solidar::ViewportCameraState extrusionCamera{
        extrusionViewport->cameraYawDegrees(),
        extrusionViewport->cameraPitchDegrees(), 1.0F, {},
        extrusionViewport->size()};
    const QPointF extrusionCap =
        extrusionCamera.worldToScreen({0.0, 0.0, 25.0});
    int sourceEvents = 0;
    bool sourceSemanticValid = false;
    bool sourceGeometryValid = false;
    QObject::connect(
        extrusionViewport, &solidar::Viewport::extrusionSourcePicked,
        extrusionViewport, [&](const solidar::ExtrusionSourcePick& pick) {
          if (const auto* face =
                  std::get_if<solidar::LegacySolidFacePick>(&pick.source)) {
            ++sourceEvents;
            sourceSemanticValid =
                (face->face == solidar::LegacySolidFace::EndCap &&
                 std::abs(face->placement.origin.z - 25.0) < 1e-9) ||
                (face->face == solidar::LegacySolidFace::InitialCap &&
                 std::abs(face->placement.origin.z) < 1e-9);
            sourceGeometryValid =
                face->geometry.semanticFingerprint() ==
                    profile.semanticFingerprint() &&
                face->geometry.semanticallyEqual(profile);
          }
        });
    extrusionViewport->beginExtrusionSurfaceSelection();
    click(*extrusionViewport, extrusionCap);
    CHECK(sourceEvents == 1);
    CHECK(sourceSemanticValid);
    CHECK(sourceGeometryValid);
    CHECK(Access::selectedExtrusionIsLegacyFace(extrusionEditor));
    CHECK(Access::legacyExtrusionDockVisible(extrusionEditor));

    // Committing a typed legacy-face pick creates the modern authoritative
    // Body and consumes the compatibility presentation. Deleting that Body
    // must leave an empty viewport, not resurrect the historical prism.
    CHECK(Access::commitLegacyExtrusionAsNewBody(extrusionEditor));
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    const auto modernBodyId = Access::firstBodyId(extrusionEditor);
    CHECK(modernBodyId != solidar::kInvalidBodyId);

    // Undo resets the viewport scene, so the compatibility mesh must be
    // reconstructed from the restored Document sketch rather than from a
    // retained geometry mirror. The next typed pick is the exact source the
    // legacy Extrude command would consume.
    Access::undoAction(extrusionEditor)->trigger();
    CHECK(Access::bodyCount(extrusionEditor) == 0);
    CHECK(Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(Access::viewportSolidVisible(extrusionEditor));
    CHECK(Access::legacySolidSketchEquals(extrusionEditor, profile));
    sourceEvents = 0;
    sourceSemanticValid = false;
    sourceGeometryValid = false;
    const solidar::ViewportCameraState undoCamera{
        extrusionViewport->cameraYawDegrees(),
        extrusionViewport->cameraPitchDegrees(), 1.0F, {},
        extrusionViewport->size()};
    extrusionViewport->beginExtrusionSurfaceSelection();
    click(*extrusionViewport,
          undoCamera.worldToScreen({0.0, 0.0, 25.0}));
    CHECK(sourceEvents == 1);
    CHECK(sourceSemanticValid);
    CHECK(sourceGeometryValid);
    CHECK(Access::selectedExtrusionIsLegacyFace(extrusionEditor));

    Access::redoAction(extrusionEditor)->trigger();
    CHECK(Access::bodyCount(extrusionEditor) == 1);
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(Access::displayedBodyShape(extrusionEditor));

    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeBody(extrusionEditor, modernBodyId);
    CHECK(Access::bodyCount(extrusionEditor) == 0);
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(!Access::viewportSolidVisible(extrusionEditor));

    // The compatibility source is part of the history transition as a stable
    // SketchId. Removing the source must clear the presentation, Undo must
    // restore the exact source, and Redo must fail closed again.
    solidar::MainWindow sourceDeletionEditor(settings);
    CHECK(sourceDeletionEditor.loadProject(legacyPath, &error));
    const auto deletedLegacySource =
        Access::historicalLegacySourceSketchId(sourceDeletionEditor);
    CHECK(deletedLegacySource);
    CHECK(Access::historicalLegacyExtrusionActive(sourceDeletionEditor));
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeHistorySketch(sourceDeletionEditor, *deletedLegacySource);
    CHECK(!Access::hasSketch(sourceDeletionEditor, *deletedLegacySource));
    CHECK(!Access::historicalLegacySourceSketchId(sourceDeletionEditor));
    CHECK(!Access::historicalLegacyExtrusionActive(sourceDeletionEditor));
    CHECK(!Access::viewportSolidVisible(sourceDeletionEditor));

    Access::undoAction(sourceDeletionEditor)->trigger();
    CHECK(Access::hasSketch(sourceDeletionEditor, *deletedLegacySource));
    CHECK(Access::historicalLegacySourceSketchId(sourceDeletionEditor) ==
          deletedLegacySource);
    CHECK(Access::historicalLegacyExtrusionActive(sourceDeletionEditor));
    CHECK(Access::viewportSolidVisible(sourceDeletionEditor));
    CHECK(Access::legacySolidSketchEquals(sourceDeletionEditor, profile));

    Access::redoAction(sourceDeletionEditor)->trigger();
    CHECK(!Access::hasSketch(sourceDeletionEditor, *deletedLegacySource));
    CHECK(!Access::historicalLegacySourceSketchId(sourceDeletionEditor));
    CHECK(!Access::historicalLegacyExtrusionActive(sourceDeletionEditor));
    CHECK(!Access::viewportSolidVisible(sourceDeletionEditor));

    // Every project replacement establishes its own presentation generation:
    // neither a modern v2 project nor an empty project may inherit v1 state.
    CHECK(extrusionEditor.loadProject(legacyPath, &error));
    CHECK(Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(extrusionEditor.loadProject(pathA, &error));
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(Access::displayedBodyShape(extrusionEditor));
    CHECK(extrusionEditor.loadProject(pathB, &error));
    CHECK(!Access::historicalLegacyExtrusionActive(extrusionEditor));
    CHECK(!Access::viewportSolidVisible(extrusionEditor));

    // `hasExtrusion=true` without an explicit source is a valid historical
    // file, but it cannot authorize a guessed box solid. The compatibility
    // path must fail closed across history refresh, a real edit/Undo cycle and
    // project replacement.
    solidar::MainWindow missingSourceEditor(settings);
    CHECK(missingSourceEditor.loadProject(missingSourcePath, &error));
    CHECK(!Access::historicalLegacyExtrusionActive(missingSourceEditor));
    CHECK(!Access::viewportSolidVisible(missingSourceEditor));
    Access::moveHistoryToEnd(missingSourceEditor);
    Access::moveHistoryToEnd(missingSourceEditor);
    CHECK(!Access::viewportSolidVisible(missingSourceEditor));
    auto* missingViewport = Access::viewport(missingSourceEditor);
    missingViewport->resize(800, 600);
    const solidar::ViewportCameraState missingCamera{
        missingViewport->cameraYawDegrees(),
        missingViewport->cameraPitchDegrees(), 1.0F, {},
        missingViewport->size()};
    const QPointF absentSolidPoint =
        missingCamera.worldToScreen({25.0, 15.0, 25.0});
    int missingLegacyPlaneEvents = 0;
    int missingExtrusionEvents = 0;
    QObject::connect(
        missingViewport, &solidar::Viewport::sketchPlanePicked,
        missingViewport, [&](const solidar::SketchPlanePick& pick) {
          if (std::holds_alternative<solidar::LegacySolidFacePick>(pick.source))
            ++missingLegacyPlaneEvents;
        });
    QObject::connect(
        missingViewport, &solidar::Viewport::extrusionSourcePicked,
        missingViewport,
        [&](const solidar::ExtrusionSourcePick&) { ++missingExtrusionEvents; });
    missingViewport->beginSketchPlaneSelection();
    click(*missingViewport, absentSolidPoint);
    CHECK(missingLegacyPlaneEvents == 0);
    missingViewport->beginExtrusionSurfaceSelection();
    click(*missingViewport, absentSolidPoint);
    CHECK(missingExtrusionEvents == 0);
    Access::cancelLegacyExtrusion(missingSourceEditor);

    const auto missingSketchId = Access::firstSketchId(missingSourceEditor);
    CHECK(missingSketchId != solidar::kInvalidSketchId);
    auto editedMissingProfile = profile;
    editedMissingProfile.addCircle({0.0, 0.0}, 2.0);
    CHECK(Access::finishSketchEdit(missingSourceEditor, missingSketchId,
                                   editedMissingProfile,
                                   solidar::SketchPlacement::xy()));
    Access::undoAction(missingSourceEditor)->trigger();
    CHECK(!Access::historicalLegacyExtrusionActive(missingSourceEditor));
    CHECK(!Access::viewportSolidVisible(missingSourceEditor));

    CHECK(missingSourceEditor.loadProject(legacyPath, &error));
    CHECK(Access::historicalLegacyExtrusionActive(missingSourceEditor));
    CHECK(Access::viewportSolidVisible(missingSourceEditor));
    CHECK(missingSourceEditor.loadProject(missingSourcePath, &error));
    CHECK(!Access::historicalLegacyExtrusionActive(missingSourceEditor));
    CHECK(!Access::viewportSolidVisible(missingSourceEditor));
  }

  // Hiding the final visible Body clears the B-Rep display. It must also turn
  // off the legacy solid fallback; otherwise the viewport draws a stale box
  // with the previous body's dimensions even though no visible Body exists.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow visibilityEditor(settings);
    CHECK(visibilityEditor.loadProject(pathA, &error));
    // Part Design operations belong to the history timeline. The model tree
    // only exposes Body rows and their visibility, without duplicating inert
    // Extrude/Move/Fillet/etc. children.
    CHECK(Access::bodyRowsHaveNoFeatureChildren(visibilityEditor));
    CHECK(Access::displayedBodyShape(visibilityEditor));
    CHECK(Access::viewportSolidVisible(visibilityEditor));
    CHECK(Access::hasDisplayableModernSolid(visibilityEditor));
    CHECK(!Access::foreignShapeIsDisplayable(visibilityEditor));
    CHECK(Access::hasExportableModernSolid(visibilityEditor));
    CHECK(Access::setFirstBodyVisible(visibilityEditor, false));
    CHECK(!Access::displayedBodyShape(visibilityEditor));
    CHECK(!Access::viewportSolidVisible(visibilityEditor));
    CHECK(!Access::hasDisplayableModernSolid(visibilityEditor));
    // Visibility is presentation state. A hidden, otherwise valid committed
    // Body remains an intentional member of the complete export snapshot.
    CHECK(Access::hasExportableModernSolid(visibilityEditor));
    CHECK(Access::setFirstBodyVisible(visibilityEditor, true));
    CHECK(Access::displayedBodyShape(visibilityEditor));
    CHECK(Access::viewportSolidVisible(visibilityEditor));
    CHECK(Access::hasDisplayableModernSolid(visibilityEditor));
  }

  // Committed history, display fallback and exportability are deliberately
  // different queries. An Error feature may retain the last validated shape
  // for presentation, but it must never authorize a stale STL export.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document errorDocument;
    auto& profile = errorDocument.addSketch("Error profile");
    profile.geometry.addRectangle({0.0, 0.0}, {12.0, 8.0});
    auto& body = errorDocument.addBody("Error body");
    auto& feature = body.addFeature(
        std::make_unique<solidar::ExtrudeFeature>(profile.id, 6.0));
    CHECK(errorDocument.recompute());
    auto* extrude = dynamic_cast<solidar::ExtrudeFeature*>(&feature);
    CHECK(extrude);
    extrude->setLengthMm(0.0);
    CHECK(!errorDocument.recompute());

    solidar::MainWindow errorEditor(settings);
    Access::installDocument(errorEditor, std::move(errorDocument));
    CHECK(Access::hasCommittedModernSolid(errorEditor));
    CHECK(Access::hasDisplayableModernSolid(errorEditor));
    QString exportError;
    CHECK(!Access::hasExportableModernSolid(errorEditor, &exportError));
    CHECK(!exportError.isEmpty());
    CHECK(Access::viewportSolidVisible(errorEditor));

    solidar::Document invalidDocument;
    auto& invalidProfile = invalidDocument.addSketch("Invalid profile");
    invalidProfile.geometry.addRectangle({0.0, 0.0}, {9.0, 7.0});
    auto& invalidBody = invalidDocument.addBody("Invalid body");
    invalidBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        invalidProfile.id, 0.0));
    CHECK(!invalidDocument.recompute());
    Access::installDocument(errorEditor, std::move(invalidDocument));
    CHECK(Access::hasCommittedModernSolid(errorEditor));
    CHECK(!Access::hasDisplayableModernSolid(errorEditor));
    exportError.clear();
    CHECK(!Access::hasExportableModernSolid(errorEditor, &exportError));
    CHECK(!exportError.isEmpty());
    CHECK(!Access::viewportSolidVisible(errorEditor));

    solidar::Document noTopologyDocument;
    auto& noTopologyBody = noTopologyDocument.addBody("No topology body");
    noTopologyBody.addFeature(
        std::make_unique<solidar::UnindexedPresentationFeature>(
            std::make_shared<const TopoDS_Shape>(
                BRepPrimAPI_MakeBox(7.0, 8.0, 9.0).Shape())));
    Access::installDocument(errorEditor, std::move(noTopologyDocument));
    CHECK(Access::hasCommittedModernSolid(errorEditor));
    CHECK(!Access::hasDisplayableModernSolid(errorEditor));
    CHECK(!Access::viewportHasBodyPresentations(errorEditor));
    CHECK(!Access::viewportSolidVisible(errorEditor));
  }

  // Entering Sketcher after a downstream Move must show the current moved
  // body without painting every consumed source sketch at its old placement.
  // Those blue upstream contours looked like a duplicate stale solid.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document document;
    auto& baseSketch = document.addSketch("Moved body source");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    const auto baseSketchId = baseSketch.id;
    auto& inspectionSketch = document.addSketch("Inspection sketch");
    inspectionSketch.geometry.addLine({0.0, 0.0}, {10.0, 0.0});
    const auto inspectionSketchId = inspectionSketch.id;
    auto& body = document.addBody("Moved body");
    auto extrusion = std::make_unique<solidar::ExtrudeFeature>(
        baseSketchId, 15.0, "Extrude");
    const auto extrusionId = extrusion->id();
    body.addFeature(std::move(extrusion));
    body.addFeature(std::make_unique<solidar::MoveFeature>(
        extrusionId, solidar::Vector3d{-80.0, 0.0, 0.0}, "Move"));
    CHECK(document.recompute());

    const QString movedPath =
        directory.filePath(QStringLiteral("moved-sketch-scene.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        movedPath, document, &error));
    solidar::MainWindow movedEditor(settings);
    CHECK(movedEditor.loadProject(movedPath, &error));
    Access::editSketch(movedEditor, inspectionSketchId);
    CHECK(Access::workspace(movedEditor)->currentWidget() ==
          Access::sketch(movedEditor));
    CHECK(Access::sketch(movedEditor)->sceneBodyCount() == 1);
    CHECK(Access::sketch(movedEditor)->sceneSketchCount() == 0);
  }

  // Headless document replacement: repeated loadProject drives the full
  // teardown path (PartDesignToolController cancelActive, session cancel,
  // resetScene, history rebuild). No file dialogs, no OpenGL surface, no
  // platform-specific code.
  solidar::MainWindow editor(settings);
  for (int cycle = 0; cycle < 6; ++cycle) {
    CHECK(editor.loadProject(pathA, &error));
    CHECK(solidar::MainWindowUndoTestAccess::displayedSketchesMatchDocument(
        editor));
    CHECK(editor.loadProject(pathB, &error));
    CHECK(solidar::MainWindowUndoTestAccess::displayedSketchesMatchDocument(
        editor));
  }

  // A rejected begin must pass through the production begin-result binder
  // before any tool UI is allowed to survive.
  for (const auto kind : {solidar::PartDesignToolKind::Fillet,
                          solidar::PartDesignToolKind::Chamfer,
                          solidar::PartDesignToolKind::Shell,
                          solidar::PartDesignToolKind::Draft})
    CHECK(solidar::MainWindowUndoTestAccess::rejectedBeginLeavesNoToolUi(
        editor, kind));

  // Save must commit the sketcher's working copy before serializing.  Without
  // this, the UI reported success while the visible sketch was absent from
  // the file on disk.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    auto* canvas = Access::sketch(editor);
    canvas->resetSketch();
    canvas->setRectangle(12.0, 8.0);
    CHECK(Access::modified(editor));
    Access::workspace(editor)->setCurrentWidget(canvas);
    Access::save(editor);
    CHECK(!Access::modified(editor));

    solidar::Document restored;
    CHECK(solidar::project::ProjectFile::loadDocument(pathB, &restored,
                                                       &error));
    CHECK(restored.sketches().size() == 1);
    CHECK(!restored.sketches().front().geometry.lines().empty());
  }

  // Window dirty follows the history revision/savepoint relation. Save moves
  // the savepoint; Undo/Redo become clean only when they return to it.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    CHECK(editor.loadProject(pathB, &error));
    CHECK(Access::pushBodyMarker(editor, "savepoint edit"));
    CHECK(Access::modified(editor));
    Access::save(editor);
    CHECK(!Access::modified(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::modified(editor));
    Access::redoAction(editor)->trigger();
    CHECK(!Access::modified(editor));

    Access::undoAction(editor)->trigger();
    Access::save(editor);
    CHECK(!Access::modified(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::modified(editor));
    Access::undoAction(editor)->trigger();
    CHECK(!Access::modified(editor));
  }

  // Switching between the legacy extrusion picker, the dedicated Revolve
  // panel and the shared Part Design panel must leave exactly one tool UI.
  // This used to reproduce after a longer session because these three paths
  // had independent teardown code.
  CHECK(editor.loadProject(pathA, &error));

  // Model and Sketch histories are separate domains. A new committed model
  // action invalidates a stale model redo, and switching workspaces exposes
  // only the active domain's availability.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    Access::reset(editor);
    const auto initialBodyCount = Access::bodyCount(editor);
    CHECK(Access::pushBodyMarker(editor, "history-domain-marker-1"));
    CHECK(Access::modified(editor));
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());
    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == initialBodyCount);
    CHECK(Access::redoCount(editor) == 1);
    CHECK(Access::redoAction(editor)->isEnabled());

    CHECK(Access::pushBodyMarker(editor, "history-domain-marker-2"));
    CHECK(Access::redoCount(editor) == 0);
    CHECK(!Access::redoAction(editor)->isEnabled());

    auto* canvas = Access::sketch(editor);
    canvas->resetSketch();
    Access::workspace(editor)->setCurrentWidget(canvas);
    QApplication::processEvents();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());

    canvas->setRectangle(12.0, 8.0);
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());
    Access::undoAction(editor)->trigger();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(Access::redoAction(editor)->isEnabled());

    Access::workspace(editor)->setCurrentWidget(Access::viewport(editor));
    QApplication::processEvents();
    CHECK(Access::undoAction(editor)->isEnabled());
    CHECK(!Access::redoAction(editor)->isEnabled());

    Access::workspace(editor)->setCurrentWidget(canvas);
    QApplication::processEvents();
    CHECK(!Access::undoAction(editor)->isEnabled());
    CHECK(Access::redoAction(editor)->isEnabled());
    Access::workspace(editor)->setCurrentWidget(Access::viewport(editor));
  }

  // Presentation-only transitions are typed history commands. Their first
  // Undo must restore presentation without falling through to the preceding
  // CAD command.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    CHECK(editor.loadProject(pathA, &error));
    Access::reset(editor);
    const std::size_t initialBodies = Access::bodyCount(editor);
    CHECK(Access::pushBodyMarker(editor, "presentation predecessor"));
    const std::size_t bodiesAfterModel = Access::bodyCount(editor);
    CHECK(bodiesAfterModel == initialBodies + 1);
    const std::size_t undoAfterModel = Access::undoCount(editor);

    CHECK(Access::originVisible(editor));
    CHECK(Access::setOriginVisible(editor, false));
    CHECK(!Access::originVisible(editor));
    CHECK(Access::undoCount(editor) == undoAfterModel + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::originVisible(editor));
    CHECK(Access::bodyCount(editor) == bodiesAfterModel);
    Access::redoAction(editor)->trigger();
    CHECK(!Access::originVisible(editor));

    const QPointF initialPosition = Access::bodyPosition(editor);
    const QPointF movedPosition(initialPosition.x() + 17.0,
                                initialPosition.y() - 9.0);
    Access::commitBodyPosition(editor, movedPosition);
    CHECK(Access::bodyPosition(editor) == movedPosition);
    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyPosition(editor) == initialPosition);
    CHECK(!Access::originVisible(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::bodyPosition(editor) == movedPosition);

    // Every typed visibility command must update the viewport and matching
    // tree checkbox together.  Exercise both transition directions so the
    // next real click cannot be consumed by stale checkbox state.
    CHECK(editor.loadProject(pathA, &error));
    Access::reset(editor);
    CHECK(Access::historyStepCount(editor) > 0);
    CHECK(Access::presentationMatchesTree(editor));
    const bool initialOriginVisible = Access::originVisible(editor);
    const std::size_t beforeOrigin = Access::undoCount(editor);
    CHECK(Access::setOriginVisible(editor, !initialOriginVisible));
    CHECK(Access::originVisible(editor) != initialOriginVisible);
    CHECK(Access::presentationMatchesTree(editor));
    CHECK(Access::undoCount(editor) == beforeOrigin + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::originVisible(editor) == initialOriginVisible);
    CHECK(Access::presentationMatchesTree(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::originVisible(editor) != initialOriginVisible);
    CHECK(Access::presentationMatchesTree(editor));
    const std::size_t beforeOriginOn = Access::undoCount(editor);
    CHECK(Access::setOriginVisible(editor, initialOriginVisible));
    CHECK(Access::undoCount(editor) == beforeOriginOn + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::originVisible(editor) != initialOriginVisible);
    Access::redoAction(editor)->trigger();
    CHECK(Access::originVisible(editor) == initialOriginVisible);
    CHECK(Access::presentationMatchesTree(editor));
    const std::size_t beforePlane = Access::undoCount(editor);
    CHECK(Access::setBasePlaneVisible(editor, 0, true));
    CHECK(Access::basePlaneVisible(editor, 0));
    CHECK(Access::presentationMatchesTree(editor));
    CHECK(Access::undoCount(editor) == beforePlane + 1);
    Access::undoAction(editor)->trigger();
    CHECK(!Access::basePlaneVisible(editor, 0));
    CHECK(Access::presentationMatchesTree(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::basePlaneVisible(editor, 0));
    CHECK(Access::presentationMatchesTree(editor));
    const std::size_t beforePlaneOff = Access::undoCount(editor);
    CHECK(Access::setBasePlaneVisible(editor, 0, false));
    CHECK(!Access::basePlaneVisible(editor, 0));
    CHECK(Access::undoCount(editor) == beforePlaneOff + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::basePlaneVisible(editor, 0));
    Access::redoAction(editor)->trigger();
    CHECK(!Access::basePlaneVisible(editor, 0));
    CHECK(Access::presentationMatchesTree(editor));

    CHECK(Access::sketchViewCount(editor) == 1);
    const bool initialSketchVisible = Access::sketchVisible(editor, 0);
    const std::size_t beforeSketch = Access::undoCount(editor);
    CHECK(Access::setSketchVisible(editor, 0, !initialSketchVisible));
    CHECK(Access::sketchVisible(editor, 0) != initialSketchVisible);
    CHECK(Access::undoCount(editor) == beforeSketch + 1);
    CHECK(Access::presentationMatchesTree(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::sketchVisible(editor, 0) == initialSketchVisible);
    CHECK(Access::presentationMatchesTree(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::sketchVisible(editor, 0) != initialSketchVisible);
    CHECK(Access::presentationMatchesTree(editor));
    const std::size_t beforeSketchBack = Access::undoCount(editor);
    CHECK(Access::setSketchVisible(editor, 0, initialSketchVisible));
    CHECK(Access::undoCount(editor) == beforeSketchBack + 1);
    Access::undoAction(editor)->trigger();
    CHECK(Access::sketchVisible(editor, 0) != initialSketchVisible);
    Access::redoAction(editor)->trigger();
    CHECK(Access::sketchVisible(editor, 0) == initialSketchVisible);
    CHECK(Access::presentationMatchesTree(editor));

    // Mixed visibility remains internally consistent while the model history
    // scrubber derives the presentation for an earlier step and returns.
    CHECK(Access::setOriginVisible(editor, true));
    CHECK(Access::setBasePlaneVisible(editor, 1, true));
    CHECK(Access::setBasePlaneVisible(editor, 2, true));
    CHECK(Access::setSketchVisible(editor, 0, true));
    CHECK(Access::presentationMatchesTree(editor));
    Access::applyHistoryPosition(editor, 0);
    CHECK(Access::presentationMatchesTree(editor));
    Access::moveHistoryToEnd(editor);
    CHECK(Access::presentationMatchesTree(editor));
  }

  // A localized model command retains only the changed Body slice. Unchanged
  // Bodies are structurally shared by omission from the delta.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document before;
    for (int index = 0; index < 200; ++index)
      before.addBody("unchanged-body-" + std::to_string(index) +
                     std::string(128, 'x'));
    solidar::Document after = before;
    after.findBody(before.bodies()[100].id())->setName("changed-body");
    const auto [sliceCount, retained] =
        Access::documentDeltaSize(before, after);
    CHECK(sliceCount == 1);
    CHECK(retained < 64U * 1024U);
    CHECK(Access::importedHistoryIsColdAndRebuildable());

    solidar::Document featureBefore;
    const auto seed = addBox(featureBefore, 0.0, 20.0, 10.0, "Delta chain");
    CHECK(seed.has_value());
    auto* chainBody = featureBefore.findBody(seed->bodyId);
    CHECK(chainBody);
    auto sourceId = seed->featureId;
    for (int index = 0; index < 80; ++index) {
      auto move = std::make_unique<solidar::MoveFeature>(
          sourceId, solidar::Vector3d{0.1, 0.0, 0.0}, "Chain move");
      sourceId = move->id();
      chainBody->addFeature(std::move(move));
    }
    CHECK(featureBefore.recompute());
    solidar::Document featureAfter = featureBefore;
    auto* changed = dynamic_cast<solidar::MoveFeature*>(
        featureAfter.findFeature(chainBody->features()[40]->id()));
    CHECK(changed);
    changed->setOffsetMm({0.2, 0.0, 0.0});
    const auto [featureSlices, featureRetained] =
        Access::documentDeltaSize(featureBefore, featureAfter);
    CHECK(featureSlices == 1);
    CHECK(featureRetained < 16U * 1024U);
  }

  // Failed command application is transactional: the source entry remains,
  // the committed Document is restored, and the RAII recursion guard drops.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    CHECK(editor.loadProject(pathA, &error));
    Access::reset(editor);
    const auto bodiesBefore = Access::bodyCount(editor);
    Access::pushThrowingUndo(editor);
    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == bodiesBefore);
    CHECK(Access::undoCount(editor) == 1);
    CHECK(Access::redoCount(editor) == 0);
    CHECK(!Access::applyingUndo(editor));
  }

  // Model-history sketch comparison is the complete persisted definition,
  // not the solver cache key. Exercise the real Finish Sketch path for a
  // construction-only change and placement-only change, then prove that an
  // unchanged finish is a no-op that does not consume history.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document sketchDocument;
    auto& item = sketchDocument.addSketch("Semantic sketch");
    item.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
    item.placement = solidar::SketchPlacement::xy();
    const auto sketchId = item.id;
    const auto baseGeometry = item.geometry;
    Access::installDocument(editor, std::move(sketchDocument));
    CHECK(Access::displayedSketchesMatchDocument(editor));

    auto construction = baseGeometry;
    construction.setLineDashedById(construction.lineId(0), true);
    CHECK(construction.solverFingerprint() == baseGeometry.solverFingerprint());
    CHECK(Access::finishSketchEdit(editor, sketchId, construction,
                                   solidar::SketchPlacement::xy()));
    CHECK(Access::documentCopy(editor).findSketch(sketchId)->geometry
              .semanticallyEqual(construction));
    const auto constructionSelection = Access::selectionState(editor);
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(Access::documentCopy(editor).findSketch(sketchId)->geometry
                .semanticallyEqual(baseGeometry));
      CHECK(Access::displayedSketchesMatchDocument(editor));
      Access::redoAction(editor)->trigger();
      CHECK(Access::documentCopy(editor).findSketch(sketchId)->geometry
                .semanticallyEqual(construction));
      CHECK(Access::displayedSketchesMatchDocument(editor));
    }
    CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                     constructionSelection));

    auto translatedPlacement = solidar::SketchPlacement::xy();
    translatedPlacement.origin = {3.0, 4.0, 5.0};
    translatedPlacement.xDirection = {0.0, 1.0, 0.0};
    translatedPlacement.yDirection = {0.0, 0.0, 1.0};
    CHECK(Access::finishSketchEdit(editor, sketchId, construction,
                                   translatedPlacement));
    auto placed = Access::documentCopy(editor);
    CHECK(placed.findSketch(sketchId)->placement.origin.x == 3.0);
    CHECK(placed.findSketch(sketchId)->placement.origin.y == 4.0);
    CHECK(placed.findSketch(sketchId)->placement.origin.z == 5.0);
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      auto unplaced = Access::documentCopy(editor);
      CHECK(unplaced.findSketch(sketchId)->placement.origin.x == 0.0);
      CHECK(unplaced.findSketch(sketchId)->placement.origin.y == 0.0);
      CHECK(unplaced.findSketch(sketchId)->placement.origin.z == 0.0);
      CHECK(Access::displayedSketchesMatchDocument(editor));
      Access::redoAction(editor)->trigger();
      placed = Access::documentCopy(editor);
      CHECK(placed.findSketch(sketchId)->placement.origin.x == 3.0);
      CHECK(placed.findSketch(sketchId)->placement.xDirection.y == 1.0);
      CHECK(placed.findSketch(sketchId)->placement.yDirection.z == 1.0);
      CHECK(Access::displayedSketchesMatchDocument(editor));
    }

    CHECK(Access::finishSketchEditNoCommand(editor, sketchId, construction,
                                            translatedPlacement));
    CHECK(Access::transientToolsInactive(editor));

    // A stale derived view must never resurrect geometry or fall back to a
    // different sketch. Both edit and presentation refresh fail closed.
    const auto committedBeforeMissingId = Access::documentCopy(editor);
    Access::injectMissingSketchViewAndRefresh(editor);
    CHECK(!Access::sketchEditActive(editor));
    CHECK(Access::displayedSketchCount(editor) == 0);
    const auto committedAfterMissingId = Access::documentCopy(editor);
    CHECK(committedAfterMissingId.sketches().size() ==
          committedBeforeMissingId.sketches().size());
    CHECK(committedAfterMissingId.findSketch(sketchId)->geometry
              .semanticallyEqual(
                  committedBeforeMissingId.findSketch(sketchId)->geometry));
    CHECK(editor.loadProject(pathA, &error));
    CHECK(Access::displayedSketchesMatchDocument(editor));
  }

  // A rejected sketch add must roll Document and the derived viewport cache
  // back atomically. The legacy top-level support string in v2 is presentation
  // compatibility data only; typed placement controls display and edit overlay.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    Access::installDocument(editor, solidar::Document{});
    solidar::sketch::Sketch profile;
    profile.addRectangle({0.0, 0.0}, {17.0, 9.0});
    auto xzPlacement = solidar::SketchPlacement::xz();
    xzPlacement.origin = {3.0, 4.0, 5.0};
    const bool modifiedBeforeRejectedSketch = Access::modified(editor);

    Access::setHistoryByteBudget(editor, 1);
    Access::finishNewSketch(
        editor, profile, xzPlacement,
        QString::fromUtf8("Произвольная подпись YZ/XY"));
    CHECK(Access::documentSketchCount(editor) == 0);
    CHECK(Access::sketchViewCount(editor) == 0);
    CHECK(Access::displayedSketchCount(editor) == 0);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::noSketchPickState(editor));
    CHECK(Access::workspace(editor)->currentWidget() == Access::sketch(editor));
    CHECK(Access::sketch(editor)->sketch().semanticallyEqual(profile));
    CHECK(Access::statusMessage(editor).contains(
        QString::fromUtf8("Операция отменена")));
    CHECK(!Access::statusMessage(editor).contains(
        QString::fromUtf8("Эскиз завершён")));
    CHECK(Access::modified(editor) == modifiedBeforeRejectedSketch);

    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);
    Access::finishNewSketch(
        editor, profile, xzPlacement,
        QString::fromUtf8("Другая произвольная подпись XY"));
    CHECK(Access::documentSketchCount(editor) == 1);
    CHECK(Access::sketchViewCount(editor) == 1);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::displayedSketchLabel(editor, 0) == QStringLiteral("XZ"));
    const auto typedSketchId = Access::firstSketchId(editor);

    const QString typedPath = directory.filePath(
        QStringLiteral("typed-xz-label-invariance.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        typedPath, Access::documentCopy(editor), &error));
    QFile typedFile(typedPath);
    CHECK(typedFile.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(typedFile.readAll()).object();
    typedFile.close();
    auto legacySketches = root.value(QStringLiteral("sketches")).toArray();
    CHECK(legacySketches.size() == 1);
    auto legacySketch = legacySketches.at(0).toObject();
    legacySketch[QStringLiteral("support")] =
        QString::fromUtf8("Неверная legacy-подпись YZ");
    legacySketches[0] = legacySketch;
    root[QStringLiteral("sketches")] = legacySketches;
    CHECK(typedFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    CHECK(typedFile.write(payload) == payload.size());
    typedFile.close();

    CHECK(editor.loadProject(typedPath, &error));
    const auto typedRoundTrip = Access::documentCopy(editor);
    const auto* restoredTyped = typedRoundTrip.findSketch(typedSketchId);
    CHECK(restoredTyped);
    CHECK(restoredTyped->placement.origin.x == 3.0);
    CHECK(restoredTyped->placement.origin.y == 4.0);
    CHECK(restoredTyped->placement.origin.z == 5.0);
    CHECK(std::abs(restoredTyped->placement.normal().y) > 0.9);
    CHECK(Access::displayedSketchLabel(editor, 0) == QStringLiteral("XZ"));
    CHECK(Access::displayedSketchesMatchDocument(editor));
    Access::editSketch(editor, typedSketchId);
    CHECK(std::abs(Access::currentSketchPlacement(editor).normal().y) > 0.9);
    CHECK(!Access::currentSketchFaceReference(editor));

    // A presentation label cannot steer picking or extrusion. Exercise an
    // arbitrary tilted plane that cannot be represented by XY/XZ/YZ strings,
    // then prove two contradictory labels yield the identical typed normal.
    CHECK(editor.loadProject(typedPath, &error));
    solidar::SketchPlacement tiltedPlacement;
    const double root2 = std::sqrt(2.0);
    const double root6 = std::sqrt(6.0);
    tiltedPlacement.origin = {7.0, -3.0, 11.0};
    tiltedPlacement.xDirection = {1.0 / root2, 1.0 / root2, 0.0};
    tiltedPlacement.yDirection = {-1.0 / root6, 1.0 / root6,
                                  2.0 / root6};
    CHECK(Access::finishSketchEdit(editor, typedSketchId, profile,
                                   tiltedPlacement));
    const QPointF directionWithXy = Access::extrusionDirectionForLabel(
        editor, 0, QString::fromUtf8("Ложная XY-подпись"), tiltedPlacement);
    const QPointF directionWithYz = Access::extrusionDirectionForLabel(
        editor, 0, QString::fromUtf8("Ложная YZ-подпись"), tiltedPlacement);
    CHECK(QLineF(directionWithXy, directionWithYz).length() < 1e-9);
    CHECK(QLineF(QPointF{}, directionWithXy).length() > 1e-3);
    Access::startSketchExtrude(editor, 0);
    CHECK(Access::directExtrudeLifecycle(editor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudePreview(editor));
    Access::reset(editor);
    CHECK(editor.loadProject(pathA, &error));
  }

  // Budget rejection is an atomic application boundary for persistent tools
  // as well as modal Pocket. Move/Extrude stay live and retryable; none of the
  // three paths publishes teardown or selection clearing before acceptance.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    for (const std::string kind : {"Move", "FaceExtrude", "Pocket"}) {
      auto fixture = makeHistoryMatrixFixture(kind);
      CHECK(fixture.has_value());
      Access::installDocument(editor, fixture->before);
      const auto beforeDocument = Access::documentCopy(editor);
      const auto beforeSelection = Access::selectionState(editor);
      Access::setHistoryByteBudget(editor, 1);
      CHECK(!Access::acceptFixtureThroughProduction(
          editor, fixture->after, fixture->featureId));
      const auto afterDocument = Access::documentCopy(editor);
      CHECK(Access::documentDeltaSize(beforeDocument, afterDocument).first ==
            0);
      CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                        beforeSelection));
      CHECK(Access::undoCount(editor) == 0);
      if (kind == "Move") {
        CHECK(Access::activeTool(editor) ==
              solidar::PartDesignToolKind::Move);
        CHECK(Access::moveLifecycle(editor) ==
              solidar::ToolLifecycle::PreviewValid);
        CHECK(Access::viewportHasToolPreview(editor));
        Access::cancelMove(editor);
      } else if (kind == "FaceExtrude") {
        CHECK(Access::activeTool(editor) ==
              solidar::PartDesignToolKind::Extrude);
        CHECK(Access::faceExtrudeLifecycle(editor) ==
              solidar::ToolLifecycle::PreviewValid);
        CHECK(Access::viewportHasToolPreview(editor));
        Access::cancelFaceExtrude(editor);
      } else {
        CHECK(Access::transientToolsInactive(editor));
      }
      Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);
    }
  }

  // STEP replacement is a hard history boundary; stale callbacks from the
  // previous document must not be reachable afterwards.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    const auto historyBeforeBoundary = Access::undoCount(editor);
    CHECK(Access::pushBodyMarker(editor, "history-boundary-marker"));
    CHECK(Access::undoCount(editor) == historyBeforeBoundary + 1);
    solidar::Document imported;
    imported.addBody("Imported body");
    Access::applyImportedDocument(editor, std::move(imported));
    CHECK(Access::undoCount(editor) == 0);
    CHECK(Access::redoCount(editor) == 0);
    CHECK(Access::retainedBytes(editor) == 0);
    CHECK(editor.loadProject(pathA, &error));
  }

  // Both history domains evict deterministically by count and retained-byte
  // budget. The model gate is driven by real imported CAD topology archived
  // into cold bytes (never by padded names or retained OCCT handles).
  {
    using Access = solidar::MainWindowUndoTestAccess;
    auto heavy = makeHeavyImportedDocument(3);
    CHECK(heavy.bodies().size() == 3);
    std::vector<solidar::BodyId> heavyIds;
    for (const auto& body : heavy.bodies()) heavyIds.push_back(body.id());
    auto oneRemoved = heavy;
    CHECK(oneRemoved.applyBodySlice(0, std::nullopt));
    const auto [heavySlices, oneCommandBytes] =
        Access::documentDeltaSize(heavy, oneRemoved);
    CHECK(heavySlices == 1);
    CHECK(oneCommandBytes > 16U * 1024U);
    Access::installDocument(editor, std::move(heavy));
    Access::setHistoryByteBudget(editor, oneCommandBytes * 2U + 4096U);
    for (const auto id : heavyIds) CHECK(Access::commitBodyRemoval(editor, id));
    CHECK(Access::retainedBytes(editor) <= oneCommandBytes * 2U + 4096U);
    CHECK(Access::undoCount(editor) < heavyIds.size());
    CHECK(Access::undoCount(editor) > 0);
    const auto countAfterRemoval = Access::bodyCount(editor);
    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == countAfterRemoval + 1);
    Access::redoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == countAfterRemoval);

    auto oversize = makeHeavyImportedDocument(1);
    CHECK(oversize.bodies().size() == 1);
    const auto oversizeId = oversize.bodies().front().id();
    solidar::Document empty;
    const auto [oversizeSlices, oversizeBytes] =
        Access::documentDeltaSize(oversize, empty);
    CHECK(oversizeSlices == 1);
    Access::installDocument(editor, std::move(oversize));
    Access::setHistoryByteBudget(editor, oversizeBytes - 1U);
    CHECK(!Access::commitBodyRemoval(editor, oversizeId));
    CHECK(Access::bodyCount(editor) == 1);
    CHECK(Access::undoCount(editor) == 0);
    CHECK(Access::retainedBytes(editor) == 0);
    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);

    auto* canvas = Access::sketch(editor);
    canvas->resetSketch();
    Access::workspace(editor)->setCurrentWidget(canvas);
    for (int index = 0; index < 140; ++index)
      canvas->setRectangle(10.0 + index, 8.0 + index);
    CHECK(canvas->undoHistorySize() == 100);
    CHECK(canvas->undoHistoryRetainedBytes() <= 32U * 1024U * 1024U);
    Access::workspace(editor)->setCurrentWidget(Access::viewport(editor));
    CHECK(editor.loadProject(pathA, &error));
  }

  // Rejecting a removal for history-budget reasons must restore a committed
  // Error feature without replaying it. Replaying the reverse delta would run
  // recompute on the broken Move and throw from the Qt slot.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document errorDocument;
    const auto valid = addBox(errorDocument, 0.0, 18.0, 12.0,
                              "Error rollback sentinel");
    CHECK(valid.has_value());
    auto* errorBody = errorDocument.findBody(valid->bodyId);
    CHECK(errorBody);
    auto brokenMove = std::make_unique<solidar::MoveFeature>(
        solidar::FeatureId{987654321},
        solidar::Vector3d{3.0, 2.0, 1.0}, "Broken rollback Move");
    const auto brokenMoveId = brokenMove->id();
    errorBody->addFeature(std::move(brokenMove));
    CHECK(!errorDocument.recompute());
    CHECK(errorDocument.findFeature(brokenMoveId));
    CHECK(errorDocument.findFeature(brokenMoveId)->state() ==
          solidar::FeatureState::Error);

    Access::installDocument(editor, std::move(errorDocument));
    CHECK(Access::selectSentinel(editor, valid->bodyId, valid->featureId, 0));
    editor.setWindowModified(false);
    const auto documentBefore = Access::documentCopy(editor);
    const auto selectionBefore = Access::selectionState(editor);
    const auto historyStepsBefore = Access::historyStepCount(editor);
    const auto undoBefore = Access::undoCount(editor);
    const auto redoBefore = Access::redoCount(editor);
    const auto retainedBefore = Access::retainedBytes(editor);
    const auto displayedBefore = Access::displayedBodyShape(editor);
    CHECK(displayedBefore);
    const double volumeBefore = solidar::test::volumeOf(*displayedBefore);

    Access::setHistoryByteBudget(editor, 1);
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeHistoryFeature(editor, valid->bodyId, brokenMoveId);

    const auto restored = Access::documentCopy(editor);
    CHECK(restored.findFeature(brokenMoveId));
    CHECK(restored.findFeature(brokenMoveId)->state() ==
          solidar::FeatureState::Error);
    auto recomputeProbe = restored;
    CHECK(!recomputeProbe.recompute());
    const auto [changedSlices, changedBytes] =
        Access::documentDeltaSize(documentBefore, restored);
    CHECK(changedSlices == 0);
    CHECK(changedBytes == 0);
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      selectionBefore));
    CHECK(Access::historyStepCount(editor) == historyStepsBefore);
    CHECK(Access::undoCount(editor) == undoBefore);
    CHECK(Access::redoCount(editor) == redoBefore);
    CHECK(Access::retainedBytes(editor) == retainedBefore);
    CHECK(!Access::modified(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));
    const auto displayedAfter = Access::displayedBodyShape(editor);
    CHECK(displayedAfter);
    CHECK(solidar::test::near(solidar::test::volumeOf(*displayedAfter),
                              volumeBefore, 1e-6));
    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);
    CHECK(editor.loadProject(pathA, &error));
  }

  // Production history handler matrix. Each fixture is a valid committed model
  // for the real feature type. Create, payload edit, and removal are applied
  // through MainWindow::pushModelTransition and exercised for two complete
  // Undo/Redo cycles, including stable selection/history restoration.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    const std::vector<std::string> kinds{
        "Extrude", "FaceExtrude", "Revolve", "Pocket", "Fillet", "Chamfer", "Shell",
        "Draft", "Join", "Mirror", "Move", "LinearPattern",
        "CircularPattern", "LinearPatternNewBody",
        "CircularPatternNewBody"};
    std::size_t kindIndex = 0;
    for (const auto& kind : kinds) {
      std::cerr << "history production matrix: " << kind << '\n';
      auto fixture = makeHistoryMatrixFixture(kind);
      CHECK(fixture.has_value());
      const std::string originalName =
          fixture->after.findFeature(fixture->featureId)->name();
      const std::string originalPayload = historyMatrixPayload(
          *fixture->after.findFeature(fixture->featureId));
      const auto expectedCreateShape = matrixShapeSignature(
          fixture->after.findFeature(fixture->featureId));
      CHECK(expectedCreateShape.has_value());

      // Create through the real production ToolSession + accept dispatcher.
      Access::installDocument(editor, fixture->before);
      if (fixture->createSelectionBodyId != solidar::kInvalidBodyId)
        Access::selectBody(editor, fixture->createSelectionBodyId);
      else
        Access::clearSelection(editor);
      const auto createBeforeSelection = Access::selectionState(editor);
      const std::size_t createBeforeBodies = Access::bodyCount(editor);
      const auto idsBefore = Access::featuresOfKind(editor, fixture->featureKind);
      const auto* referenceJoin = dynamic_cast<const solidar::JoinBodiesFeature*>(
          fixture->after.findFeature(fixture->featureId));
      CHECK(Access::acceptFixtureThroughProduction(
          editor, fixture->after, fixture->featureId));
      const auto idsAfter = Access::featuresOfKind(editor, fixture->featureKind);
      CHECK(idsAfter.size() == idsBefore.size() + 1);
      const auto createdId = *std::find_if(
          idsAfter.begin(), idsAfter.end(), [&idsBefore](solidar::FeatureId id) {
            return std::find(idsBefore.begin(), idsBefore.end(), id) ==
                   idsBefore.end();
          });
      const auto* createdFeature = Access::feature(editor, createdId);
      CHECK(createdFeature != nullptr);
      CHECK(historyMatrixPayload(*createdFeature, false) ==
            historyMatrixPayload(
                *fixture->after.findFeature(fixture->featureId), false));
      const auto createdShape = matrixShapeSignature(createdFeature);
      CHECK(createdShape.has_value());
      CHECK(sameMatrixShape(*createdShape, *expectedCreateShape));
      CHECK(matrixReferencesResolve(Access::documentCopy(editor),
                                    *createdFeature));
      const std::string createdPayload = historyMatrixPayload(*createdFeature);
      const bool createsBody =
          fixture->before.findBody(fixture->bodyId) == nullptr;
      CHECK(Access::bodyCount(editor) ==
            createBeforeBodies + static_cast<std::size_t>(createsBody));
      if (createsBody)
        CHECK(Access::featureOwner(editor, createdId) !=
              solidar::kInvalidBodyId);
      else
        CHECK(Access::featureOwner(editor, createdId) == fixture->bodyId);
      CHECK(Access::historyContainsFeature(editor, createdId));
      const auto createAfterSelection = Access::selectionState(editor);
      CHECK(createAfterSelection.atEnd);
      CHECK(Access::transientToolsInactive(editor));
      if (referenceJoin) {
        CHECK(!Access::bodyVisible(editor, referenceJoin->firstBodyId()));
        CHECK(!Access::bodyVisible(editor, referenceJoin->secondBodyId()));
      }
      for (int cycle = 0; cycle < 2; ++cycle) {
        const auto* created = Access::feature(editor, createdId);
        CHECK(created && created->kind() == fixture->featureKind);
        CHECK(historyMatrixPayload(*created) == createdPayload);
        const auto currentShape = matrixShapeSignature(created);
        CHECK(currentShape && sameMatrixShape(*currentShape, *createdShape));
        CHECK(Access::historyAtEnd(editor));
        CHECK(Access::transientToolsInactive(editor));
        Access::undoAction(editor)->trigger();
        CHECK(!Access::feature(editor, createdId));
        CHECK(Access::bodyCount(editor) == createBeforeBodies);
        if (referenceJoin) {
          CHECK(Access::bodyVisible(editor, referenceJoin->firstBodyId()));
          CHECK(Access::bodyVisible(editor, referenceJoin->secondBodyId()));
        }
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         createBeforeSelection));
        CHECK(Access::historyAtEnd(editor));
        CHECK(Access::transientToolsInactive(editor));
        Access::redoAction(editor)->trigger();
        CHECK(Access::feature(editor, createdId));
        const auto* redone = Access::feature(editor, createdId);
        CHECK(historyMatrixPayload(*redone) == createdPayload);
        const auto redoneShape = matrixShapeSignature(redone);
        CHECK(redoneShape && sameMatrixShape(*redoneShape, *createdShape));
        CHECK(matrixReferencesResolve(Access::documentCopy(editor), *redone));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         createAfterSelection));
        CHECK(Access::transientToolsInactive(editor));
        if (referenceJoin) {
          CHECK(!Access::bodyVisible(editor, referenceJoin->firstBodyId()));
          CHECK(!Access::bodyVisible(editor, referenceJoin->secondBodyId()));
        }
      }

      // Accepting an unchanged supported editor is a precise application/UI
      // no-op.  Rotate raw Body/Edge/Face sentinels and the initial modified
      // flag across the complete 15-variant production matrix.  A second real
      // open+Cancel proves the empty-delta path did not leave stale editor
      // transaction state behind.
      Access::installDocument(editor, fixture->after);
      CHECK(Access::selectSentinel(editor, fixture->sentinelBodyId,
                                   fixture->sentinelFeatureId,
                                   static_cast<int>(kindIndex % 3)));
      const bool unchangedModified = (kindIndex % 2) != 0;
      editor.setWindowModified(unchangedModified);
      const auto unchangedSelection = Access::selectionState(editor);
      const auto unchangedUndo = Access::undoCount(editor);
      const auto unchangedRedo = Access::redoCount(editor);
      const auto unchangedHistory = Access::historyStepCount(editor);
      const auto unchangedBodies = Access::bodyCount(editor);
      const auto unchangedPayload = historyMatrixPayload(
          *Access::feature(editor, fixture->featureId));
      const auto unchangedShape = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      const auto unchangedDefinition = serializedDocumentDefinition(
          Access::documentCopy(editor));
      CHECK(unchangedShape.has_value());
      CHECK(unchangedDefinition.has_value());
      CHECK(Access::acceptUnchangedFeatureEditThroughProduction(
          editor, fixture->bodyId, fixture->featureId));
      const auto definitionAfterUnchanged = serializedDocumentDefinition(
          Access::documentCopy(editor));
      CHECK(definitionAfterUnchanged.has_value());
      if (*definitionAfterUnchanged != *unchangedDefinition)
        std::cerr << "unchanged Accept document mismatch for " << kind << ": "
                  << firstSerializedDocumentDifference(
                         *unchangedDefinition, *definitionAfterUnchanged)
                         .toStdString()
                  << '\n';
      CHECK(*definitionAfterUnchanged == *unchangedDefinition);
      CHECK(historyMatrixPayload(
                *Access::feature(editor, fixture->featureId)) ==
            unchangedPayload);
      const auto shapeAfterUnchanged = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      CHECK(shapeAfterUnchanged &&
            sameMatrixShape(*shapeAfterUnchanged, *unchangedShape));
      CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                        unchangedSelection));
      CHECK(Access::modified(editor) == unchangedModified);
      CHECK(Access::undoCount(editor) == unchangedUndo);
      CHECK(Access::redoCount(editor) == unchangedRedo);
      CHECK(Access::historyStepCount(editor) == unchangedHistory);
      CHECK(Access::bodyCount(editor) == unchangedBodies);
      CHECK(!Access::editTransactionActive(editor));
      CHECK(Access::transientToolsInactive(editor));
      CHECK(Access::cancelFeatureEditThroughProduction(
          editor, fixture->bodyId, fixture->featureId));
      CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                        unchangedSelection));
      CHECK(Access::modified(editor) == unchangedModified);
      CHECK(Access::undoCount(editor) == unchangedUndo);
      CHECK(Access::redoCount(editor) == unchangedRedo);
      CHECK(!Access::editTransactionActive(editor));
      CHECK(Access::transientToolsInactive(editor));

      // Edit through the real history dispatcher and the production session
      // accept path (modal Extrude/Pocket editors are auto-accepted).
      Access::installDocument(editor, fixture->after);
      CHECK(Access::selectSentinel(editor, fixture->sentinelBodyId,
                                   fixture->sentinelFeatureId,
                                   static_cast<int>(kindIndex % 3)));
      const auto cancelPayload = historyMatrixPayload(
          *Access::feature(editor, fixture->featureId));
      const auto cancelShape = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      CHECK(cancelShape.has_value());
      CHECK(Access::cancelFeatureEditThroughProduction(
          editor, fixture->bodyId, fixture->featureId));
      CHECK(historyMatrixPayload(*Access::feature(editor, fixture->featureId)) ==
            cancelPayload);
      const auto shapeAfterCancel = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      CHECK(shapeAfterCancel && sameMatrixShape(*shapeAfterCancel,
                                                *cancelShape));
      const auto originalShape = matrixShapeSignature(
          Access::feature(editor, fixture->featureId));
      CHECK(originalShape.has_value());
      const auto expectedEdited = makeExpectedEditedDocument(*fixture);
      CHECK(expectedEdited.has_value());
      const auto* expectedEditedFeature =
          expectedEdited->findFeature(fixture->featureId);
      CHECK(expectedEditedFeature != nullptr);
      const std::string expectedEditedPayload =
          historyMatrixPayload(*expectedEditedFeature);
      const auto expectedEditedShape =
          matrixShapeSignature(expectedEditedFeature);
      CHECK(expectedEditedShape.has_value());
      CHECK(matrixReferencesResolve(*expectedEdited, *expectedEditedFeature));
      CHECK(Access::editFeatureThroughProduction(
          editor, fixture->bodyId, fixture->featureId));
      const auto committedEditBeforeSelection =
          Access::expectedEditUndoSelection();
      CHECK(committedEditBeforeSelection.has_value());
      const auto* editedFeature = Access::feature(editor, fixture->featureId);
      CHECK(editedFeature);
      const std::string editedPayload = historyMatrixPayload(*editedFeature);
      CHECK(editedPayload == expectedEditedPayload);
      CHECK(editedPayload != originalPayload);
      const auto editedShape = matrixShapeSignature(editedFeature);
      CHECK(editedShape.has_value());
      CHECK(sameMatrixShape(*editedShape, *expectedEditedShape));
      CHECK(matrixReferencesResolve(Access::documentCopy(editor),
                                    *editedFeature));
      const auto editAfterSelection = Access::selectionState(editor);
      CHECK(editAfterSelection.atEnd);
      CHECK(Access::transientToolsInactive(editor));
      for (int cycle = 0; cycle < 2; ++cycle) {
        CHECK(historyMatrixPayload(
                  *Access::feature(editor, fixture->featureId)) ==
              expectedEditedPayload);
        const auto currentEditedShape = matrixShapeSignature(
            Access::feature(editor, fixture->featureId));
        CHECK(currentEditedShape &&
              sameMatrixShape(*currentEditedShape, *expectedEditedShape));
        Access::undoAction(editor)->trigger();
        const auto* original = Access::feature(editor, fixture->featureId);
        CHECK(original && original->kind() == fixture->featureKind);
        CHECK(historyMatrixPayload(*original) == originalPayload);
        const auto restoredShape = matrixShapeSignature(original);
        CHECK(restoredShape && sameMatrixShape(*restoredShape, *originalShape));
        CHECK(matrixReferencesResolve(Access::documentCopy(editor), *original));
        CHECK(Access::editUndoSelectionRestored(editor));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         *committedEditBeforeSelection));
        CHECK(Access::historyAtEnd(editor));
        CHECK(Access::transientToolsInactive(editor));
        Access::redoAction(editor)->trigger();
        const auto* redone = Access::feature(editor, fixture->featureId);
        CHECK(redone && historyMatrixPayload(*redone) ==
                            expectedEditedPayload);
        const auto redoneShape = matrixShapeSignature(redone);
        CHECK(redoneShape &&
              sameMatrixShape(*redoneShape, *expectedEditedShape));
        CHECK(matrixReferencesResolve(Access::documentCopy(editor), *redone));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         editAfterSelection));
        CHECK(Access::transientToolsInactive(editor));
      }

      // Removal goes through the real history-row removal path and its
      // dependency/visibility policy.
      Access::installDocument(editor, fixture->after);
      Access::selectBody(editor, fixture->bodyId);
      const auto removeBeforeSelection = Access::selectionState(editor);
      CHECK(Access::removeFeatureThroughProduction(editor,
                                                   fixture->featureId));
      const auto removeAfterSelection = Access::selectionState(editor);
      CHECK(Access::transientToolsInactive(editor));
      if (referenceJoin) {
        CHECK(Access::bodyVisible(editor, referenceJoin->firstBodyId()));
        CHECK(Access::bodyVisible(editor, referenceJoin->secondBodyId()));
      }
      for (int cycle = 0; cycle < 2; ++cycle) {
        CHECK(!Access::feature(editor, fixture->featureId));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         removeAfterSelection));
        Access::undoAction(editor)->trigger();
        const auto* restored = Access::feature(editor, fixture->featureId);
        if (!restored) {
          std::cerr << "missing restored id " << fixture->featureId
                    << " undo=" << Access::undoCount(editor)
                    << " redo=" << Access::redoCount(editor)
                    << " status=" << editor.statusBar()->currentMessage().toStdString()
                    << '\n';
        }
        CHECK(restored && restored->kind() == fixture->featureKind);
        CHECK(restored->name() == originalName);
        CHECK(historyMatrixPayload(*restored) == originalPayload);
        const auto restoredShape = matrixShapeSignature(restored);
        CHECK(restoredShape && sameMatrixShape(*restoredShape,
                                               *expectedCreateShape));
        CHECK(matrixReferencesResolve(Access::documentCopy(editor), *restored));
        if (referenceJoin) {
          CHECK(!Access::bodyVisible(editor, referenceJoin->firstBodyId()));
          CHECK(!Access::bodyVisible(editor, referenceJoin->secondBodyId()));
        }
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         removeBeforeSelection));
        CHECK(Access::transientToolsInactive(editor));
        Access::redoAction(editor)->trigger();
        CHECK(!Access::feature(editor, fixture->featureId));
        CHECK(Access::sameSelectionState(Access::selectionState(editor),
                                         removeAfterSelection));
        CHECK(Access::transientToolsInactive(editor));
        if (referenceJoin) {
          CHECK(Access::bodyVisible(editor, referenceJoin->firstBodyId()));
          CHECK(Access::bodyVisible(editor, referenceJoin->secondBodyId()));
        }
      }
      ++kindIndex;
    }
    CHECK(editor.loadProject(pathA, &error));
  }

  // Mirror plane highlighting is transient tool UI.  A pre-visible user plane
  // must survive unchanged Apply with redo intact, and a real accepted edit
  // must carry that same presentation through Undo/Redo.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    auto fixture = makeHistoryMatrixFixture("Mirror");
    CHECK(fixture.has_value());
    Access::installDocument(editor, fixture->after);
    Access::reset(editor);
    CHECK(Access::setBasePlaneVisible(editor, 1, true));
    CHECK(Access::setOriginVisible(editor, false));
    Access::undoAction(editor)->trigger();
    CHECK(Access::basePlaneVisible(editor, 1));
    CHECK(Access::originVisible(editor));
    CHECK(Access::redoCount(editor) == 1);
    CHECK(Access::presentationMatchesTree(editor));

    const auto unchangedBefore = Access::selectionState(editor);
    const auto unchangedUndo = Access::undoCount(editor);
    const auto unchangedRedo = Access::redoCount(editor);
    const bool unchangedDirty = Access::modified(editor);
    CHECK(Access::acceptUnchangedFeatureEditThroughProduction(
        editor, fixture->bodyId, fixture->featureId));
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      unchangedBefore));
    CHECK(Access::undoCount(editor) == unchangedUndo);
    CHECK(Access::redoCount(editor) == unchangedRedo);
    CHECK(Access::modified(editor) == unchangedDirty);
    CHECK(Access::basePlaneVisible(editor, 1));
    CHECK(Access::presentationMatchesTree(editor));

    const auto editedPresentation =
        Access::selectionState(editor).presentation;
    CHECK(Access::editFeatureThroughProduction(
        editor, fixture->bodyId, fixture->featureId));
    CHECK(Access::selectionState(editor).presentation == editedPresentation);
    CHECK(Access::basePlaneVisible(editor, 1));
    CHECK(Access::presentationMatchesTree(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::selectionState(editor).presentation == editedPresentation);
    CHECK(Access::presentationMatchesTree(editor));
    Access::redoAction(editor)->trigger();
    CHECK(Access::selectionState(editor).presentation == editedPresentation);
    CHECK(Access::presentationMatchesTree(editor));
    CHECK(editor.loadProject(pathA, &error));
  }

  // Edit dispatch is atomic even when a clickable history row cannot open an
  // editor. Unsupported features and every invalid-source early return must
  // restore the exact raw selection/history/modified state and leave no stale
  // transaction that could contaminate the next unrelated tool.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    const auto expectAtomicNoEditor = [&](solidar::BodyId bodyId,
                                          solidar::FeatureId featureId) {
      const auto beforeSelection = Access::selectionState(editor);
      const bool beforeModified = Access::modified(editor);
      const auto beforeUndo = Access::undoCount(editor);
      const auto beforeRedo = Access::redoCount(editor);
      const auto beforeBodies = Access::bodyCount(editor);
      const auto beforeHistory = Access::historyStepCount(editor);
      Access::editHistoryFeature(editor, bodyId, featureId);
      return !Access::editTransactionActive(editor) &&
             Access::exactSelectionState(Access::selectionState(editor),
                                         beforeSelection) &&
             Access::modified(editor) == beforeModified &&
             Access::undoCount(editor) == beforeUndo &&
             Access::redoCount(editor) == beforeRedo &&
             Access::bodyCount(editor) == beforeBodies &&
             Access::historyStepCount(editor) == beforeHistory &&
             Access::transientToolsInactive(editor);
    };

    solidar::Document unsupported;
    const auto unsupportedSentinel = addBox(
        unsupported, -50.0, -30.0, 8.0, "Unsupported sentinel");
    CHECK(unsupportedSentinel.has_value());
    auto& importedBody = unsupported.addBody("Imported unsupported");
    auto imported = std::make_unique<solidar::ImportedShapeFeature>(
        std::make_shared<const TopoDS_Shape>(
            BRepPrimAPI_MakeBox(8.0, 7.0, 6.0).Shape()),
        "Imported unsupported");
    const auto importedBodyId = importedBody.id();
    const auto importedId = imported->id();
    importedBody.addFeature(std::move(imported));
    CHECK(unsupported.recompute());
    Access::installDocument(editor, std::move(unsupported));
    CHECK(Access::selectSentinel(editor, unsupportedSentinel->bodyId,
                                 unsupportedSentinel->featureId, 2));
    editor.setWindowModified(false);
    CHECK(expectAtomicNoEditor(importedBodyId, importedId));

    // Missing feature is an earlier no-op boundary and must be equally clean.
    CHECK(Access::selectSentinel(editor, unsupportedSentinel->bodyId,
                                 unsupportedSentinel->featureId, 1));
    CHECK(expectAtomicNoEditor(importedBodyId,
                               solidar::kInvalidFeatureId));

    // A known type with no preceding/source feature reaches an early return
    // inside editPatternFeature after the UI transaction was captured.
    solidar::Document missingSource;
    const auto missingSourceSentinel = addBox(
        missingSource, -40.0, -20.0, 6.0, "Missing source sentinel");
    CHECK(missingSourceSentinel.has_value());
    auto& brokenBody = missingSource.addBody("Broken move owner");
    auto brokenMove = std::make_unique<solidar::MoveFeature>(
        solidar::FeatureId{987654}, solidar::Vector3d{1.0, 2.0, 3.0},
        "Broken Move");
    const auto brokenBodyId = brokenBody.id();
    const auto brokenMoveId = brokenMove->id();
    brokenBody.addFeature(std::move(brokenMove));
    Access::installDocument(editor, std::move(missingSource));
    CHECK(Access::selectSentinel(editor, missingSourceSentinel->bodyId,
                                 missingSourceSentinel->featureId, 0));
    editor.setWindowModified(true);
    CHECK(expectAtomicNoEditor(brokenBodyId, brokenMoveId));

    // Join has an explicit history-at-end precondition. The dispatch guard
    // must restore the stable non-end history key instead of leaking the
    // transaction when that precondition rejects the editor.
    auto joinFixture = makeHistoryMatrixFixture("Join");
    CHECK(joinFixture.has_value());
    Access::installDocument(editor, joinFixture->after);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::selectSentinel(editor, joinFixture->sentinelBodyId,
                                 joinFixture->sentinelFeatureId, 2));
    CHECK(Access::historyStepCount(editor) > 0);
    Access::applyHistoryPosition(editor, 0);
    CHECK(!Access::historyAtEnd(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));
    editor.setWindowModified(false);
    CHECK(expectAtomicNoEditor(joinFixture->bodyId,
                               joinFixture->featureId));
    CHECK(Access::historyPosition(editor) == 0);

    // An invalid persistent topology reference is still a successfully open
    // editor when the session remains active (PreviewInvalid): the transaction
    // must be retained until the real Cancel path restores the pre-tool state.
    auto faceFixture = makeHistoryMatrixFixture("FaceExtrude");
    CHECK(faceFixture.has_value());
    auto invalidTopology = faceFixture->before;
    const auto* validFaceFeature = dynamic_cast<const solidar::ExtrudeFeature*>(
        faceFixture->after.findFeature(faceFixture->featureId));
    CHECK(validFaceFeature && validFaceFeature->faceReference());
    auto invalidFace = *validFaceFeature->faceReference();
    invalidFace.faceIndex = 999999;
    invalidFace.persistentTag = "missing-face";
    invalidFace.signature.reset();
    auto* invalidOwner = invalidTopology.findBody(faceFixture->bodyId);
    CHECK(invalidOwner != nullptr);
    auto invalidExtrude = std::make_unique<solidar::ExtrudeFeature>(
        invalidFace, validFaceFeature->lengthMm(), "Invalid topology edit",
        validFaceFeature->operation(), validFaceFeature->reversed());
    const auto invalidExtrudeId = invalidExtrude->id();
    invalidOwner->addFeature(std::move(invalidExtrude));
    Access::installDocument(editor, std::move(invalidTopology));
    CHECK(Access::selectSentinel(editor, faceFixture->sentinelBodyId,
                                 faceFixture->sentinelFeatureId, 1));
    editor.setWindowModified(false);
    const auto invalidBeforeSelection = Access::selectionState(editor);
    Access::editHistoryFeature(editor, faceFixture->bodyId, invalidExtrudeId);
    CHECK(Access::editTransactionActive(editor));
    CHECK(Access::faceExtrudeEditingFeature(editor) == invalidExtrudeId);
    CHECK(Access::faceExtrudeLifecycle(editor) ==
          solidar::ToolLifecycle::PreviewInvalid);
    Access::cancelFaceExtrude(editor);
    CHECK(!Access::editTransactionActive(editor));
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      invalidBeforeSelection));
    CHECK(!Access::modified(editor));
    CHECK(Access::transientToolsInactive(editor));

    // A subsequent valid editor proves failed dispatches left no stale state.
    auto moveFixture = makeHistoryMatrixFixture("Move");
    CHECK(moveFixture.has_value());
    Access::installDocument(editor, moveFixture->after);
    CHECK(Access::selectSentinel(editor, moveFixture->sentinelBodyId,
                                 moveFixture->sentinelFeatureId, 2));
    editor.setWindowModified(false);
    const auto supportedBeforeSelection = Access::selectionState(editor);
    Access::editHistoryFeature(editor, moveFixture->bodyId,
                               moveFixture->featureId);
    CHECK(Access::editTransactionActive(editor));
    CHECK(Access::moveEditingFeature(editor) == moveFixture->featureId);
    CHECK(Access::moveLifecycle(editor) != solidar::ToolLifecycle::Inactive);
    const auto activeEditorSelection = Access::selectionState(editor);
    Access::editHistoryFeature(editor, moveFixture->bodyId,
                               solidar::kInvalidFeatureId);
    CHECK(Access::editTransactionActive(editor));
    CHECK(Access::moveEditingFeature(editor) == moveFixture->featureId);
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      activeEditorSelection));
    Access::cancelMove(editor);
    CHECK(!Access::editTransactionActive(editor));
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      supportedBeforeSelection));
    CHECK(!Access::modified(editor));
    CHECK(Access::transientToolsInactive(editor));
    CHECK(editor.loadProject(pathA, &error));
  }

  // Join visibility is a shared-consumer relation, not ownership. J1 and J2
  // both consume A; editing/removing only one of them must not expose A while
  // the other survives. Direct Body removal uses the same policy as history
  // feature removal, and Cancel is an exact no-op.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document shared;
    const auto a = addBox(shared, 0.0, 20.0, 10.0, "Shared A");
    const auto b = addBox(shared, 10.0, 30.0, 10.0, "Shared B");
    const auto c = addBox(shared, 5.0, 25.0, 10.0, "Shared C");
    const auto d = addBox(shared, 10.0, 30.0, 10.0, "Shared D");
    CHECK(a && b && c && d);
    auto& j1Body = shared.addBody("J1 result");
    const auto j1BodyId = j1Body.id();
    auto j1 = std::make_unique<solidar::JoinBodiesFeature>(
        a->bodyId, a->featureId, b->bodyId, b->featureId, "J1");
    const auto j1Id = j1->id();
    j1Body.addFeature(std::move(j1));
    auto& j2Body = shared.addBody("J2 result");
    const auto j2BodyId = j2Body.id();
    auto j2 = std::make_unique<solidar::JoinBodiesFeature>(
        a->bodyId, a->featureId, c->bodyId, c->featureId, "J2");
    const auto j2Id = j2->id();
    j2Body.addFeature(std::move(j2));
    shared.findBody(a->bodyId)->setVisible(false);
    shared.findBody(b->bodyId)->setVisible(false);
    shared.findBody(c->bodyId)->setVisible(false);
    CHECK(shared.recompute());
    Access::installDocument(editor, shared);

    const std::string cancelPayload = historyMatrixPayload(
        *Access::feature(editor, j1Id));
    CHECK(Access::cancelJoinEdit(editor, j1BodyId, j1Id, a->bodyId,
                                 b->bodyId));
    CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) ==
          cancelPayload);
    CHECK(!Access::bodyVisible(editor, a->bodyId));
    CHECK(!Access::bodyVisible(editor, b->bodyId));
    CHECK(!Access::bodyVisible(editor, c->bodyId));

    // Removing the whole J1 result Body releases B, but A remains consumed by
    // J2. Undo re-creates J1 and re-hides B; Redo repeats the exact policy.
    QTimer::singleShot(0, [] {
      if (auto* box = qobject_cast<QMessageBox*>(
              QApplication::activeModalWidget()))
        box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, j1BodyId);
    CHECK(!Access::feature(editor, j1Id));
    CHECK(!Access::bodyVisible(editor, a->bodyId));
    CHECK(Access::bodyVisible(editor, b->bodyId));
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(Access::feature(editor, j1Id));
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(!Access::bodyVisible(editor, b->bodyId));
      Access::redoAction(editor)->trigger();
      CHECK(!Access::feature(editor, j1Id));
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(Access::bodyVisible(editor, b->bodyId));
    }
    Access::undoAction(editor)->trigger();
    CHECK(Access::feature(editor, j1Id));

    CHECK(Access::editJoinOperands(editor, j1BodyId, j1Id, d->bodyId,
                                   b->bodyId));
    const std::string editedJ1 = historyMatrixPayload(
        *Access::feature(editor, j1Id));
    CHECK(!Access::bodyVisible(editor, a->bodyId));
    CHECK(!Access::bodyVisible(editor, b->bodyId));
    CHECK(!Access::bodyVisible(editor, c->bodyId));
    CHECK(!Access::bodyVisible(editor, d->bodyId));
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) ==
            cancelPayload);
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(Access::bodyVisible(editor, d->bodyId));
      Access::redoAction(editor)->trigger();
      CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) == editedJ1);
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(!Access::bodyVisible(editor, d->bodyId));
    }

    CHECK(Access::removeFeatureThroughProduction(editor, j2Id));
    CHECK(Access::bodyVisible(editor, a->bodyId));
    CHECK(Access::bodyVisible(editor, c->bodyId));
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(Access::feature(editor, j2Id));
      CHECK(!Access::bodyVisible(editor, a->bodyId));
      CHECK(!Access::bodyVisible(editor, c->bodyId));
      Access::redoAction(editor)->trigger();
      CHECK(!Access::feature(editor, j2Id));
      CHECK(Access::bodyVisible(editor, a->bodyId));
      CHECK(Access::bodyVisible(editor, c->bodyId));
    }
    const QString sharedPath =
        directory.filePath(QStringLiteral("shared-join-visibility.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        sharedPath, Access::documentCopy(editor), &error));
    solidar::Document restoredShared;
    CHECK(solidar::project::ProjectFile::loadDocument(
        sharedPath, &restoredShared, &error));
    Access::installDocument(editor, std::move(restoredShared));
    CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) == editedJ1);
    CHECK(!Access::feature(editor, j2Id));
    CHECK(Access::bodyVisible(editor, a->bodyId));
    CHECK(Access::bodyVisible(editor, c->bodyId));
    CHECK(!Access::bodyVisible(editor, b->bodyId));
    CHECK(!Access::bodyVisible(editor, d->bodyId));

    // With the last Join consumer gone, direct result-Body removal releases
    // both operands. The same visibility and payload survive two cycles.
    QTimer::singleShot(0, [] {
      if (auto* box = qobject_cast<QMessageBox*>(
              QApplication::activeModalWidget()))
        box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, j1BodyId);
    CHECK(!Access::feature(editor, j1Id));
    CHECK(Access::bodyVisible(editor, b->bodyId));
    CHECK(Access::bodyVisible(editor, d->bodyId));
    for (int cycle = 0; cycle < 2; ++cycle) {
      Access::undoAction(editor)->trigger();
      CHECK(historyMatrixPayload(*Access::feature(editor, j1Id)) == editedJ1);
      CHECK(!Access::bodyVisible(editor, b->bodyId));
      CHECK(!Access::bodyVisible(editor, d->bodyId));
      CHECK(Access::transientToolsInactive(editor));
      Access::redoAction(editor)->trigger();
      CHECK(!Access::feature(editor, j1Id));
      CHECK(Access::bodyVisible(editor, b->bodyId));
      CHECK(Access::bodyVisible(editor, d->bodyId));
      CHECK(Access::transientToolsInactive(editor));
    }
    CHECK(Access::transientToolsInactive(editor));
    CHECK(j2BodyId != solidar::kInvalidBodyId);
    CHECK(editor.loadProject(pathA, &error));
  }

  auto* filletButton = editor.findChild<QToolButton*>(
      QStringLiteral("filletCommand"));
  auto* extrudeButton = editor.findChild<QToolButton*>(
      QStringLiteral("extrudeCommand"));
  auto* revolveButton = editor.findChild<QToolButton*>(
      QStringLiteral("revolveCommand"));
  auto* partDesignDock = editor.findChild<QDockWidget*>(
      QStringLiteral("partDesignParametersDock"));
  auto* extrusionDock = editor.findChild<QDockWidget*>(
      QStringLiteral("extrusionParametersDock"));
  auto* revolveDock = editor.findChild<QDockWidget*>(
      QStringLiteral("revolveParametersDock"));
  CHECK(filletButton != nullptr);
  CHECK(extrudeButton != nullptr);
  CHECK(revolveButton != nullptr);
  CHECK(partDesignDock != nullptr);
  CHECK(extrusionDock != nullptr);
  CHECK(revolveDock != nullptr);

  filletButton->click();
  QApplication::processEvents();
  CHECK(!partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(revolveDock->isHidden());

  extrudeButton->click();
  QApplication::processEvents();
  CHECK(partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());  // still selecting an input profile
  CHECK(revolveDock->isHidden());

  revolveButton->click();
  QApplication::processEvents();
  CHECK(partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(!revolveDock->isHidden());

  filletButton->click();
  QApplication::processEvents();
  CHECK(!partDesignDock->isHidden());
  CHECK(extrusionDock->isHidden());
  CHECK(revolveDock->isHidden());

  // Active-tool teardown at the controller/session layer. A live Fillet
  // session holds B-Rep references and a preview; the same cancelActive +
  // cancel sequence MainWindow::loadProject performs must leave the controller
  // and session fully inactive, with no surviving preview or selection state.
  {
    solidar::Document document;
    auto& baseSketch = document.addSketch("Base");
    baseSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 20.0});
    auto& body = document.addBody("Body");
    auto extrude = std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 15.0, "Extrude");
    const solidar::FeatureId featureId = extrude->id();
    body.addFeature(std::move(extrude));
    CHECK(document.recompute());
    const auto shape = body.resultShape();
    CHECK(shape != nullptr);

    solidar::PartDesignToolController controller;
    solidar::FilletToolSession fillet;
    controller.registerTool(solidar::PartDesignToolKind::Fillet,
                            {&fillet});
    controller.activate(solidar::PartDesignToolKind::Fillet);
    CHECK(controller.activeTool() == solidar::PartDesignToolKind::Fillet);
    CHECK(controller.activeSession() == &fillet);

    // Zero radius keeps the preview equal to the base shape, so the session is
    // deterministically active and holding B-Rep without depending on whether
    // a particular edge is fillet-able.
    fillet.begin(body.id(), featureId, shape,
                 {solidar::EdgeReference{body.id(), featureId, 0}}, 0.0);
    CHECK(fillet.lifecycle() != solidar::ToolLifecycle::Inactive);
    CHECK(fillet.previewShape() != nullptr);

    // Mirror the teardown order of MainWindow::loadProject.
    controller.cancelActive();
    fillet.cancel();

    CHECK(controller.activeTool() == solidar::PartDesignToolKind::None);
    CHECK(controller.activeSession() == nullptr);
    CHECK(fillet.lifecycle() == solidar::ToolLifecycle::Inactive);
    CHECK(fillet.previewShape() == nullptr);
    CHECK(fillet.edges().empty());
  }

  // End-to-end direct Sketch-on-Face Extrude: signed drag chooses outward
  // Join or inward Cut, commit creates one history entry, transient state is
  // cleared, and Undo/Redo plus history scrubbing preserve an editable model.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document document;
    auto& baseSketch = document.addSketch("Direct Extrude base");
    baseSketch.geometry.addRectangle({-20.0, -15.0}, {20.0, 15.0});
    auto& body = document.addBody("Direct Extrude body");
    auto base = std::make_unique<solidar::ExtrudeFeature>(
        baseSketch.id, 20.0, "Extrude");
    const auto baseId = base->id();
    body.addFeature(std::move(base));
    CHECK(document.recompute());
    const auto topFace =
        solidar::test::topPlanarFace(*body.resultShape(), 20.0);
    CHECK(topFace);
    const auto support = solidar::makeFaceReference(
        *body.resultShape(), body.id(), baseId, *topFace);
    auto& faceSketch = document.addSketch("Direct Extrude face profile");
    CHECK(document.attachSketchToFace(faceSketch.id, support));
    faceSketch.geometry.addRectangle({-5.0, -5.0}, {5.0, 5.0});
    CHECK(document.recompute());

    const QString directPath =
        directory.filePath(QStringLiteral("direct-face-extrude.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        directPath, document, &error));
    solidar::MainWindow directEditor(settings);
    CHECK(directEditor.loadProject(directPath, &error));
    directEditor.show();
    directEditor.activateWindow();
    QApplication::processEvents();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    const auto baseShape = Access::bodyShape(directEditor);
    CHECK(baseShape);
    const double baseVolume = solidar::test::volumeOf(*baseShape);

    Access::startSketchExtrude(directEditor, 1);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudeOperation(directEditor) ==
          solidar::ExtrudeOperation::Join);
    CHECK(!Access::directExtrudeReversed(directEditor));
    CHECK(Access::directExtrudePreview(directEditor));
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::activeTool(directEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Escape));
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::Inactive);
    CHECK(!Access::directExtrudePreview(directEditor));
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    CHECK(Access::undoCount(directEditor) == 0);

    Access::startSketchExtrude(directEditor, 1);
    Access::dragDirectExtrude(directEditor, 8.0);
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Return));
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 0);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::Inactive);
    CHECK(!Access::directExtrudePreview(directEditor));
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(Access::activeTool(directEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(Access::viewport(directEditor)->selectedBodyFaces().empty());
    CHECK(Access::viewport(directEditor)->selectionFilter() ==
          solidar::SelectionFilter::Any);
    CHECK(!Access::viewport(directEditor)->linearToolManipulator());
    const auto joinedShape = Access::bodyShape(directEditor);
    CHECK(joinedShape);
    CHECK(solidar::test::solidCount(*joinedShape) == 1);
    const double joinedVolume = solidar::test::volumeOf(*joinedShape);
    CHECK(joinedVolume > baseVolume);

    // A second Enter/Apply after completion is a no-op, not a duplicate node.
    Access::commitDirectExtrudeFromViewport(directEditor);
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);

    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 1);
    Access::redoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::bodyShape(directEditor)),
        joinedVolume, 1e-4));

    CHECK(Access::historyStepCount(directEditor) == 4);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        joinedVolume, 1e-4));
    Access::applyHistoryPosition(directEditor, 2);
    CHECK(!Access::historyAtEnd(directEditor));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        baseVolume, 1e-4));
    Access::moveHistoryToEnd(directEditor);
    CHECK(Access::historyAtEnd(directEditor));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::displayedBodyShape(directEditor)),
        joinedVolume, 1e-4));

    // Returning to the tip must restore an immediately editable model.
    CHECK(Access::startTopFaceExtrude(directEditor, 28.0));
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::viewportHasToolPreview(directEditor));
    CHECK(Access::pressDirectExtrudeKey(directEditor, Qt::Key_Escape));
    CHECK(!Access::viewportHasToolPreview(directEditor));

    // Return to the base and create the inward Cut through the same UI path.
    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    Access::startSketchExtrude(directEditor, 1);
    Access::dragDirectExtrude(directEditor, -6.0);
    CHECK(Access::directExtrudeLifecycle(directEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::directExtrudeOperation(directEditor) ==
          solidar::ExtrudeOperation::Cut);
    CHECK(Access::directExtrudeReversed(directEditor));
    CHECK(solidar::test::volumeOf(
              *Access::directExtrudePreview(directEditor)) < baseVolume);
    CHECK(Access::viewportHasToolPreview(directEditor));
    Access::commitDirectExtrudeFromViewport(directEditor);
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(Access::undoCount(directEditor) == 1);
    CHECK(Access::redoCount(directEditor) == 0);
    const auto cutShape = Access::bodyShape(directEditor);
    CHECK(cutShape);
    CHECK(solidar::test::solidCount(*cutShape) == 1);
    const double cutVolume = solidar::test::volumeOf(*cutShape);
    CHECK(cutVolume < baseVolume);
    CHECK(!Access::viewportHasToolPreview(directEditor));
    CHECK(!Access::viewport(directEditor)->linearToolManipulator());
    CHECK(Access::viewport(directEditor)->selectedBodyFaces().empty());

    Access::undoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    Access::redoAction(directEditor)->trigger();
    CHECK(Access::bodyFeatureCount(directEditor) == 2);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*Access::bodyShape(directEditor)), cutVolume,
        1e-4));

    // Deleting a step through its real context menu must let the menu and the
    // originating button finish their event handlers before rebuilding the
    // timeline. Rebuilding synchronously used to destroy both objects while
    // QMenu::exec() and customContextMenuRequested were still on the stack.
    const auto historyButtons =
        directEditor.findChildren<QToolButton*>(QStringLiteral("historyStep"));
    QToolButton* featureButton = nullptr;
    for (QToolButton* button : historyButtons)
      if (button->property("featureId").toULongLong() !=
          solidar::kInvalidFeatureId)
        featureButton = button;
    CHECK(featureButton != nullptr);
    QPointer<QToolButton> deletedButton(featureButton);
    QTimer contextMenuDriver;
    bool deleteChosen = false;
    QObject::connect(&contextMenuDriver, &QTimer::timeout, [&] {
      if (!deleteChosen)
        for (QWidget* widget : QApplication::allWidgets())
          if (auto* menu = qobject_cast<QMenu*>(widget))
            for (QAction* action : menu->actions())
              if (action->text() == QString::fromUtf8("Удалить")) {
                deleteChosen = true;
                menu->setActiveAction(action);
                QKeyEvent choose(QEvent::KeyPress, Qt::Key_Return,
                                 Qt::NoModifier);
                QApplication::sendEvent(menu, &choose);
                return;
              }
      for (QWidget* widget : QApplication::allWidgets()) {
        if (auto* box = qobject_cast<QMessageBox*>(widget)) {
          box->done(QMessageBox::Yes);
          return;
        }
      }
    });
    contextMenuDriver.start(1);
    const bool menuInvoked = QMetaObject::invokeMethod(
        featureButton, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, QPoint(4, 4)));
    QApplication::processEvents();
    contextMenuDriver.stop();
    CHECK(menuInvoked);
    CHECK(deletedButton.isNull());
    CHECK(Access::bodyFeatureCount(directEditor) == 1);
    CHECK(Access::historyStepCount(directEditor) == 3);
  }

  // Removing a Body must synchronize the derived sketch view while modern
  // profile consumption remains resolved from stable Feature dependencies.
  // Exercise the real confirmation plus Undo/Redo.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document twoBodies;
    auto& firstSketch = twoBodies.addSketch("First profile");
    firstSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    auto& firstBody = twoBodies.addBody("First body");
    const auto removedBodyId = firstBody.id();
    firstBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        firstSketch.id, 5.0));
    auto& secondSketch = twoBodies.addSketch("Second profile");
    secondSketch.geometry.addRectangle({30.0, 0.0}, {42.0, 10.0});
    auto& secondBody = twoBodies.addBody("Second body");
    secondBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        secondSketch.id, 7.0));
    CHECK(twoBodies.recompute());
    const QString twoBodiesPath =
        directory.filePath(QStringLiteral("two-bodies-delete.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        twoBodiesPath, twoBodies, &error));
    CHECK(editor.loadProject(twoBodiesPath, &error));
    CHECK(Access::bodyCount(editor) == 2);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));

    Access::setHistoryByteBudget(editor, 1);
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, removedBodyId);
    CHECK(Access::bodyCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::statusMessage(editor).contains(
        QString::fromUtf8("Операция отменена")));
    CHECK(!Access::statusMessage(editor).contains(
        QString::fromUtf8("Body удалён")));
    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);

    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeBody(editor, removedBodyId);
    CHECK(Access::bodyCount(editor) == 1);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::hasCommittedModernSolid(editor));
    CHECK(Access::hasDisplayableModernSolid(editor));
    CHECK(Access::hasExportableModernSolid(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));

    Access::undoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == 2);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::hasCommittedModernSolid(editor));
    CHECK(Access::hasDisplayableModernSolid(editor));
    CHECK(Access::hasExportableModernSolid(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));

    Access::redoAction(editor)->trigger();
    CHECK(Access::bodyCount(editor) == 1);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::hasCommittedModernSolid(editor));
    CHECK(Access::hasDisplayableModernSolid(editor));
    CHECK(Access::hasExportableModernSolid(editor));
    CHECK(Access::displayedSketchesMatchDocument(editor));
  }

  // A Feature cascade can remove a face-supported Sketch and Features in
  // other Bodies. The derived UI view must shrink before tree/history
  // rebuild and Undo must restore the same stable dependency graph.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::Document cascade;
    auto& baseSketch = cascade.addSketch("Cascade base");
    const auto baseSketchId = baseSketch.id;
    baseSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    auto& supportBody = cascade.addBody("Cascade support");
    const auto supportBodyId = supportBody.id();
    auto& supportFeature = supportBody.addFeature(
        std::make_unique<solidar::ExtrudeFeature>(baseSketchId, 10.0));
    const auto supportFeatureId = supportFeature.id();
    CHECK(cascade.recompute());
    const auto topFace =
        solidar::test::topPlanarFace(*supportBody.resultShape(), 10.0);
    CHECK(topFace);
    auto& faceSketch = cascade.addSketch("Cascade face sketch");
    const auto faceSketchId = faceSketch.id;
    faceSketch.geometry.addRectangle({2.0, 2.0}, {8.0, 8.0});
    CHECK(cascade.attachSketchToFace(
        faceSketchId, {supportBodyId, supportFeatureId, *topFace}));
    auto& consumerBody = cascade.addBody("Cascade consumer");
    const auto consumerBodyId = consumerBody.id();
    consumerBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        faceSketchId, 4.0));
    CHECK(cascade.recompute());

    const QString cascadePath =
        directory.filePath(QStringLiteral("feature-cascade-delete.solidar"));
    CHECK(solidar::project::ProjectFile::saveDocument(
        cascadePath, cascade, &error));
    CHECK(editor.loadProject(cascadePath, &error));
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    const auto loadedCascade = Access::documentCopy(editor);
    const auto* loadedFaceSketch = loadedCascade.findSketch(faceSketchId);
    CHECK(loadedFaceSketch);
    CHECK(loadedFaceSketch->support.type ==
          solidar::SketchSupportType::Face);
    CHECK(loadedFaceSketch->support.face.bodyId == supportBodyId);
    CHECK(loadedFaceSketch->support.face.featureId == supportFeatureId);
    Access::editSketch(editor, faceSketchId);
    const auto editFaceReference = Access::currentSketchFaceReference(editor);
    CHECK(editFaceReference);
    CHECK(editFaceReference->bodyId == supportBodyId);
    CHECK(editFaceReference->featureId == supportFeatureId);
    CHECK(editor.loadProject(cascadePath, &error));
    const auto bodyPresentationBeforeRollback =
        Access::displayedBodyShape(editor);
    CHECK(bodyPresentationBeforeRollback);
    const double bodyVolumeBeforeRollback =
        solidar::test::volumeOf(*bodyPresentationBeforeRollback);
    CHECK(Access::sketchConsumedByModernFeature(editor, faceSketchId));
    const auto rollbackSelectionBefore = Access::selectionState(editor);

    // Force production history-budget rejection. The delete path must not
    // remove a viewport row or disturb positional picks before acceptance.
    Access::setHistoryByteBudget(editor, 1);
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeHistoryFeature(editor, supportBodyId, supportFeatureId);
    CHECK(Access::hasSketch(editor, baseSketchId));
    CHECK(Access::hasSketch(editor, faceSketchId));
    CHECK(Access::bodyFeatureCount(editor, supportBodyId) == 1);
    CHECK(Access::bodyFeatureCount(editor, consumerBodyId) == 1);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::sketchConsumedByModernFeature(editor, faceSketchId));
    CHECK(Access::exactSelectionState(Access::selectionState(editor),
                                      rollbackSelectionBefore));
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::noSketchPickState(editor));
    const auto bodyPresentationAfterRollback =
        Access::displayedBodyShape(editor);
    CHECK(bodyPresentationAfterRollback);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*bodyPresentationAfterRollback),
        bodyVolumeBeforeRollback, 1e-6));
    Access::setHistoryByteBudget(editor, 128U * 1024U * 1024U);

    Access::setRevolveAxisSketchIndex(editor, 1);
    CHECK(Access::revolveAxisSketchIndex(editor) == 1);
    QTimer::singleShot(0, [] {
      for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget))
          box->done(QMessageBox::Yes);
    });
    Access::removeHistoryFeature(editor, supportBodyId, supportFeatureId);
    CHECK(Access::hasSketch(editor, baseSketchId));
    CHECK(!Access::hasSketch(editor, faceSketchId));
    CHECK(Access::bodyFeatureCount(editor, supportBodyId) == 0);
    CHECK(Access::bodyFeatureCount(editor, consumerBodyId) == 0);
    CHECK(Access::sketchViewCount(editor) == 1);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(!Access::sketchConsumedByModernFeature(editor, faceSketchId));
    CHECK(Access::noSketchPickState(editor));

    Access::undoAction(editor)->trigger();
    CHECK(Access::hasSketch(editor, baseSketchId));
    CHECK(Access::hasSketch(editor, faceSketchId));
    CHECK(Access::bodyFeatureCount(editor, supportBodyId) == 1);
    CHECK(Access::bodyFeatureCount(editor, consumerBodyId) == 1);
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::sketchConsumedByModernFeature(editor, faceSketchId));
    CHECK(Access::noSketchPickState(editor));
    Access::redoAction(editor)->trigger();
    CHECK(!Access::hasSketch(editor, faceSketchId));
    CHECK(Access::sketchViewCount(editor) == 1);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::noSketchPickState(editor));
    Access::undoAction(editor)->trigger();
    CHECK(Access::hasSketch(editor, faceSketchId));
    CHECK(Access::sketchViewCount(editor) == 2);
    CHECK(Access::displayedSketchesMatchDocument(editor));
    CHECK(Access::noSketchPickState(editor));
  }

  // A corrupt v2 must fail before the transactional project-switch boundary:
  // the old model, path, dirty flag and active preview/session all survive.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow transactionalEditor(settings);
    CHECK(transactionalEditor.loadProject(pathA, &error));
    CHECK(Access::startTopFaceExtrude(transactionalEditor, 15.0));
    Access::dragDirectExtrude(transactionalEditor, 4.0);
    CHECK(Access::activeTool(transactionalEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::directExtrudeLifecycle(transactionalEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::viewportHasToolPreview(transactionalEditor));
    transactionalEditor.setWindowModified(true);

    const QString oldPath = transactionalEditor.windowFilePath();
    const std::size_t oldBodyCount = Access::bodyCount(transactionalEditor);
    const std::size_t oldHistoryCount =
        Access::historyStepCount(transactionalEditor);
    const auto oldShape = Access::bodyShape(transactionalEditor);
    CHECK(oldShape && !oldShape->IsNull());
    const double oldVolume = solidar::test::volumeOf(*oldShape);

    QFile validFile(pathA);
    CHECK(validFile.open(QIODevice::ReadOnly));
    auto corruptRoot = QJsonDocument::fromJson(validFile.readAll()).object();
    corruptRoot.remove("model");
    const QString corruptPath =
        directory.filePath(QStringLiteral("corrupt-switch.solidar"));
    QFile corruptFile(corruptPath);
    CHECK(corruptFile.open(QIODevice::WriteOnly));
    const QByteArray corruptBytes =
        QJsonDocument(corruptRoot).toJson(QJsonDocument::Compact);
    CHECK(corruptFile.write(corruptBytes) == corruptBytes.size());
    corruptFile.close();

    CHECK(!transactionalEditor.loadProject(corruptPath, &error));
    CHECK(!error.isEmpty());
    CHECK(transactionalEditor.windowFilePath() == oldPath);
    CHECK(Access::modified(transactionalEditor));
    CHECK(Access::bodyCount(transactionalEditor) == oldBodyCount);
    CHECK(Access::historyStepCount(transactionalEditor) == oldHistoryCount);
    CHECK(Access::activeTool(transactionalEditor) ==
          solidar::PartDesignToolKind::Extrude);
    CHECK(Access::directExtrudeLifecycle(transactionalEditor) ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(Access::viewportHasToolPreview(transactionalEditor));
    const auto preservedShape = Access::bodyShape(transactionalEditor);
    CHECK(preservedShape && !preservedShape->IsNull());
    CHECK(solidar::test::near(solidar::test::volumeOf(*preservedShape),
                              oldVolume, 1e-6));
    QFile preservedFile(corruptPath);
    CHECK(preservedFile.open(QIODevice::ReadOnly));
    CHECK(preservedFile.readAll() == corruptBytes);
  }

  // Import is also a Document replacement boundary. A pending NewBody/legacy
  // preview has no Body/Feature source id, so generation invalidation (not
  // source lookup) must stop it before the old B-Rep graph is destroyed.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow importEditor(settings);
    CHECK(importEditor.loadProject(pathA, &error));
    int rebuilds = 0;
    int publications = 0;
    Access::queueInvalidSourcePreview(importEditor, &rebuilds, &publications);
    solidar::Document imported;
    imported.setBox({123.0, 45.0, 67.0});
    auto& importedSketch = imported.addSketch("Imported presentation sketch");
    importedSketch.geometry.addRectangle({1.0, 2.0}, {8.0, 11.0});
    importedSketch.placement = solidar::SketchPlacement::xz();
    Access::applyImportedDocument(importEditor, std::move(imported));
    QApplication::processEvents(QEventLoop::AllEvents);
    CHECK(rebuilds == 0);
    CHECK(publications == 0);
    CHECK(Access::displayedSketchesMatchDocument(importEditor));
  }

  // The legacy operation detector is keyed by semantic inputs. Repeated
  // scheduling for the same body/revision/profile executes once, and Cancel
  // invalidates a queued detector before it can inspect or publish stale UI.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow legacyEditor(settings);
    CHECK(legacyEditor.loadProject(pathA, &error));
    const auto initialCount =
        Access::automaticExtrudeDetectionCount(legacyEditor);
    Access::scheduleAutomaticExtrudeDetection(legacyEditor);
    Access::scheduleAutomaticExtrudeDetection(legacyEditor);
    Access::dispatchPreviewCadence(legacyEditor);
    CHECK(Access::automaticExtrudeDetectionCount(legacyEditor) ==
          initialCount + 1);
    Access::scheduleAutomaticExtrudeDetection(legacyEditor);
    Access::dispatchPreviewCadence(legacyEditor);
    CHECK(Access::automaticExtrudeDetectionCount(legacyEditor) ==
          initialCount + 1);

    // Reset the semantic key, then enqueue one genuinely pending detection.
    Access::cancelLegacyExtrusion(legacyEditor);
    Access::exposeLegacyExtrusion(legacyEditor);
    Access::scheduleAutomaticExtrudeDetection(legacyEditor);
    Access::cancelLegacyExtrusion(legacyEditor);
    Access::dispatchPreviewCadence(legacyEditor);
    CHECK(Access::automaticExtrudeDetectionCount(legacyEditor) ==
          initialCount + 1);
    CHECK(!Access::legacyExtrusionDockVisible(legacyEditor));
    CHECK(!Access::legacyExtrusionSelectionActive(legacyEditor));
  }

  // Body visibility is a model/view generation boundary. It must tear down
  // both a live tool preview and queued work before mutating the visible set.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow visibilityEditor(settings);
    CHECK(visibilityEditor.loadProject(pathA, &error));
    CHECK(Access::startTopFaceExtrude(visibilityEditor, 15.0));
    CHECK(Access::viewportHasToolPreview(visibilityEditor));
    int rebuilds = 0;
    int publications = 0;
    Access::queueInvalidSourcePreview(visibilityEditor, &rebuilds,
                                      &publications);
    CHECK(Access::setFirstBodyVisible(visibilityEditor, false));
    Access::dispatchPreviewCadence(visibilityEditor);
    CHECK(Access::activeTool(visibilityEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(!Access::viewportHasToolPreview(visibilityEditor));
    CHECK(rebuilds == 0);
    CHECK(publications == 0);
  }

  // Return commits the text currently being edited, not the previous spinbox
  // value: interpretText -> coalesced preview flush -> exact model commit.
  {
    using Access = solidar::MainWindowUndoTestAccess;
    solidar::MainWindow moveEditor(settings);
    CHECK(moveEditor.loadProject(pathA, &error));
    moveEditor.show();
    QApplication::processEvents();
    const std::size_t featureCount = Access::bodyFeatureCount(moveEditor);
    CHECK(Access::startMoveForActiveBody(moveEditor));
    CHECK(Access::enterMoveXAndAccept(moveEditor, QStringLiteral("12")));
    QApplication::processEvents();
    CHECK(Access::bodyFeatureCount(moveEditor) == featureCount + 1);
    const auto moveOffset = Access::lastMoveOffset(moveEditor);
    CHECK(moveOffset.has_value());
    CHECK(solidar::test::near(moveOffset->x, 12.0, 1e-9));
    CHECK(solidar::test::near(moveOffset->y, 0.0, 1e-9));
    CHECK(solidar::test::near(moveOffset->z, 0.0, 1e-9));
    CHECK(Access::activeTool(moveEditor) ==
          solidar::PartDesignToolKind::None);
    CHECK(!Access::viewportHasToolPreview(moveEditor));
  }

  // Closing a modified project must never discard work without an explicit
  // choice. Cancel keeps the editor alive; Discard permits the close.
  CHECK(editor.loadProject(pathA, &error));
  CHECK(solidar::MainWindowUndoTestAccess::pushBodyMarker(
      editor, "close dirty sentinel"));
  editor.show();
  QApplication::processEvents();
  CHECK(solidar::MainWindowUndoTestAccess::modified(editor));
  QTimer::singleShot(0, [] {
    for (QWidget* widget : QApplication::topLevelWidgets())
      if (auto* box = qobject_cast<QMessageBox*>(widget))
        box->done(QMessageBox::Cancel);
  });
  CHECK(!editor.close());
  CHECK(editor.isVisible());

  QTimer::singleShot(0, [] {
    for (QWidget* widget : QApplication::topLevelWidgets())
      if (auto* box = qobject_cast<QMessageBox*>(widget))
        box->done(QMessageBox::Discard);
  });
  CHECK(editor.close());

  return EXIT_SUCCESS;
}
