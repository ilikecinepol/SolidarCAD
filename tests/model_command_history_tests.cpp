#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/PocketFeature.h"
#include "model/RevolveFeature.h"
#include "model/ShellFeature.h"
#include "ui/application/ModelCommandHistory.h"
#include "TestGeometryUtils.h"

namespace {

solidar::Document namedDocument(std::string name) {
  solidar::Document document;
  document.addBody(std::move(name));
  return document;
}

solidar::EditorCommittedState state(int position, bool modified) {
  static_cast<void>(modified);
  solidar::EditorCommittedState result;
  result.sketchViewIds = {11, 12};
  result.bodies = {21};
  result.historyBodyId = 21;
  result.historyFeatureId = 31;
  result.historySketchId = 11;
  result.historyPosition = position;
  result.atEnd = false;
  result.historicalLegacyExtrusionSourceSketchId = 12;
  return result;
}

std::vector<std::unique_ptr<solidar::ShapeFeature>> allFeatureKinds() {
  using namespace solidar;
  std::vector<std::unique_ptr<ShapeFeature>> features;
  features.push_back(std::make_unique<ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(
          BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape()),
      "Imported"));
  features.push_back(std::make_unique<ExtrudeFeature>(1, 10.0, "Extrude"));
  features.push_back(std::make_unique<RevolveFeature>(
      1, AxisReference{AxisReferenceType::GlobalX}, 180.0, "Revolve"));
  features.push_back(std::make_unique<PocketFeature>(1, 5.0, "Pocket"));
  features.push_back(
      std::make_unique<FilletFeature>(EdgeReference{}, 1.0, "Fillet"));
  features.push_back(
      std::make_unique<ChamferFeature>(EdgeReference{}, 1.0, "Chamfer"));
  features.push_back(
      std::make_unique<MirrorFeature>(1, MirrorPlane::XY, "Mirror"));
  features.push_back(std::make_unique<MoveFeature>(
      1, Vector3d{1.0, 2.0, 3.0}, "Move"));
  features.push_back(std::make_unique<LinearPatternFeature>(
      1, PrincipalAxis::X, 2, 10.0, "Linear pattern"));
  features.push_back(std::make_unique<CircularPatternFeature>(
      1, PrincipalAxis::Z, 3, 270.0, "Circular pattern"));
  features.push_back(
      std::make_unique<JoinBodiesFeature>(1, 2, 3, 4, "Join"));
  features.push_back(std::make_unique<ShellFeature>(
      1, std::vector<FaceReference>{FaceReference{}}, 1.0, false, "Shell"));
  features.push_back(std::make_unique<DraftFeature>(
      1, std::vector<FaceReference>{FaceReference{}}, PlaneReference{},
      AxisReference{AxisReferenceType::GlobalZ}, 3.0, false, "Draft"));
  return features;
}

}  // namespace

int main() {
  using namespace solidar;

  // The service owns the complete symmetric command lifecycle and returns
  // plain stable state without touching any QObject or QWidget.
  {
    Document before = namedDocument("before");
    Document current = before;
    current.findBody(current.bodies().front().id())->setName("after");
    const auto beforeState = state(2, false);
    const auto afterState = state(7, true);
    ModelCommandHistory history;
    auto commit = history.recordTransition(
        current, std::move(before), beforeState, afterState);
    CHECK(commit.status == HistoryCommitStatus::Accepted);
    CHECK(history.undoCount() == 1);
    CHECK(history.redoCount() == 0);

    for (int cycle = 0; cycle < 2; ++cycle) {
      const auto undo = history.undo(current, afterState);
      CHECK(undo.changed);
      CHECK(undo.error.empty());
      CHECK(current.bodies().front().name() == "before");
      CHECK(undo.state == beforeState);
      CHECK(history.undoCount() == 0);
      CHECK(history.redoCount() == 1);

      // An empty accepted edit is not a command and must retain Redo.
      Document unchanged = current;
      const auto empty = history.recordTransition(
          current, std::move(unchanged), beforeState, beforeState);
      CHECK(empty.status == HistoryCommitStatus::NoChange);
      CHECK(history.redoCount() == 1);

      const auto redo = history.redo(current, beforeState);
      CHECK(redo.changed);
      CHECK(redo.error.empty());
      CHECK(current.bodies().front().name() == "after");
      CHECK(redo.state == afterState);
      CHECK(history.undoCount() == 1);
      CHECK(history.redoCount() == 0);
    }
  }

  // Oversized transitions are rejected transactionally: both Document and
  // the caller-visible committed state point back to the exact before-state.
  {
    Document before = namedDocument("before-budget");
    Document current = before;
    current.findBody(current.bodies().front().id())
        ->setName(std::string(2048, 'x'));
    const auto beforeState = state(1, false);
    ModelCommandHistory history(1);
    const auto result = history.recordTransition(
        current, std::move(before), beforeState, state(9, true));
    CHECK(result.status == HistoryCommitStatus::Rejected);
    CHECK(current.bodies().front().name() == "before-budget");
    CHECK(result.state == beforeState);
    CHECK(history.undoCount() == 0);
    CHECK(history.retainedBytes() == 0);
  }

  // Count eviction is deterministic and keeps the newest bounded tail.
  {
    Document current = namedDocument("0");
    ModelCommandHistory history;
    for (std::size_t index = 1; index <= ModelCommandHistory::kMaxCommands + 5;
         ++index) {
      Document before = current;
      current.findBody(current.bodies().front().id())
          ->setName(std::to_string(index));
      const auto result = history.recordTransition(
          current, std::move(before), state(static_cast<int>(index - 1), true),
          state(static_cast<int>(index), true));
      CHECK(result.status == HistoryCommitStatus::Accepted);
    }
    CHECK(history.undoCount() == ModelCommandHistory::kMaxCommands);
  }

  // Recompute failure during Undo leaves the source entry, Document and
  // recursion guard intact. The failing before-state contains a broken Move;
  // the current state is valid after removing it.
  {
    Document invalid = namedDocument("failure");
    Body& body = *invalid.findBody(invalid.bodies().front().id());
    auto broken = std::make_unique<MoveFeature>(
        FeatureId{987654321}, Vector3d{1.0, 0.0, 0.0}, "broken");
    const FeatureId brokenId = broken->id();
    body.addFeature(std::move(broken));
    CHECK(!invalid.recompute());
    Document current = invalid;
    const auto index = current.bodies().front().featureIndex(brokenId);
    CHECK(index.has_value());
    CHECK(current.applyFeatureSlice(current.bodies().front().id(), *index,
                                    nullptr));
    CHECK(current.recompute());

    ModelCommandHistory history;
    const auto committed = history.recordTransition(
        current, std::move(invalid), state(1, false), state(2, true));
    CHECK(committed.status == HistoryCommitStatus::Accepted);
    const auto rollbackName = current.bodies().front().name();
    const auto undo = history.undo(current, committed.state);
    CHECK(!undo.changed);
    CHECK(!undo.error.empty());
    CHECK(current.bodies().front().name() == rollbackName);
    CHECK(current.bodies().front().features().empty());
    CHECK(history.undoCount() == 1);
    CHECK(history.redoCount() == 0);
    CHECK(!history.isApplying());
  }

  // Every persisted FeatureKind is represented by the history descriptor
  // registry. An unchanged clone must not create a false-positive delta.
  {
    auto features = allFeatureKinds();
    CHECK(features.size() == kPersistedFeatureKindCount);
    for (auto& feature : features) {
      Document before;
      before.addBody("registry").addFeature(std::move(feature));
      Document after = before;
      const auto unchanged = ModelCommandHistory::inspectDelta(before, after);
      CHECK(unchanged.sliceCount == 0);
      after.findFeature(after.bodies().front().features().front()->id())
          ->setName("changed");
      const auto changed = ModelCommandHistory::inspectDelta(before, after);
      CHECK(changed.sliceCount == 1);
    }
  }

  // Dirty state is a property of the stable history revision versus the
  // explicit savepoint, not a boolean copied into individual commands.
  {
    Document current = namedDocument("root");
    ModelCommandHistory history;
    CHECK(history.isAtSavepoint());

    Document root = current;
    current.findBody(current.bodies().front().id())->setName("saved-edit");
    CHECK(history.recordTransition(current, std::move(root), state(0, false),
                                   state(1, true)).status ==
          HistoryCommitStatus::Accepted);
    CHECK(!history.isAtSavepoint());
    history.markSavepoint();
    CHECK(history.isAtSavepoint());

    CHECK(history.undo(current, state(1, true)).changed);
    CHECK(!history.isAtSavepoint());  // edit -> save -> undo
    CHECK(history.redo(current, state(0, false)).changed);
    CHECK(history.isAtSavepoint());   // returning to savepoint is clean

    CHECK(history.undo(current, state(1, true)).changed);
    history.markSavepoint();          // undo -> save
    CHECK(history.isAtSavepoint());
    CHECK(history.redo(current, state(0, false)).changed);
    CHECK(!history.isAtSavepoint());  // redo leaves the new savepoint
    CHECK(history.undo(current, state(1, true)).changed);
    CHECK(history.isAtSavepoint());

    Document branchBase = current;
    current.findBody(current.bodies().front().id())->setName("branch");
    CHECK(history.recordTransition(current, std::move(branchBase),
                                   state(0, false), state(2, true)).status ==
          HistoryCommitStatus::Accepted);
    CHECK(!history.canRedo());
    CHECK(!history.isAtSavepoint());  // unreachable redo savepoint stays dirty
  }

  // Presentation-only transitions are first-class typed commands. They must
  // not fall through to the previous CAD command on Ctrl+Z.
  {
    Document current;
    ModelCommandHistory history;
    EditorCommittedState before;
    EditorCommittedState after;
    after.presentation.originVisible = false;
    const auto commit = history.recordTransition(
        current, Document(current), before, after);
    CHECK(commit.status == HistoryCommitStatus::Accepted);
    CHECK(history.undoCount() == 1);
    const auto undo = history.undo(current, after);
    CHECK(undo.changed);
    CHECK(undo.state.presentation.originVisible);
    const auto redo = history.redo(current, before);
    CHECK(redo.changed);
    CHECK(!redo.state.presentation.originVisible);
  }

  // Imported geometry equality includes the archived B-Rep payload. Replacing
  // a feature with the same ID/name but different shape is a real command.
  {
    Document before;
    Body& body = before.addBody("imported");
    auto original = std::make_unique<ImportedShapeFeature>(
        std::make_shared<const TopoDS_Shape>(
            BRepPrimAPI_MakeBox(2.0, 3.0, 4.0).Shape()),
        "Import");
    const FeatureId id = original->id();
    body.addFeature(std::move(original));
    CHECK(before.recompute());

    Document current = before;
    CHECK(current.applyFeatureSlice(
        current.bodies().front().id(), 0,
        std::make_unique<ImportedShapeFeature>(
            id, std::make_shared<const TopoDS_Shape>(
                    BRepPrimAPI_MakeBox(5.0, 3.0, 4.0).Shape()),
            "Import").get()));
    CHECK(current.recompute());
    CHECK(ModelCommandHistory::inspectDelta(before, current).sliceCount == 1);

    ModelCommandHistory history;
    CHECK(history.recordTransition(current, std::move(before), {}, {}).status ==
          HistoryCommitStatus::Accepted);
    CHECK(history.undo(current, {}).changed);
    const auto* restored = dynamic_cast<const ImportedShapeFeature*>(
        current.bodies().front().features().front().get());
    CHECK(restored && restored->shape());
    CHECK(test::near(test::volumeOf(*restored->shape()), 24.0));

    Document branchBase = current;
    CHECK(current.applyFeatureSlice(
        current.bodies().front().id(), 0,
        std::make_unique<ImportedShapeFeature>(
            id, std::make_shared<const TopoDS_Shape>(
                    BRepPrimAPI_MakeBox(7.0, 3.0, 4.0).Shape()),
            "Import").get()));
    CHECK(current.recompute());
    CHECK(history.recordTransition(current, std::move(branchBase), {}, {}).status ==
          HistoryCommitStatus::Accepted);
    CHECK(!history.canRedo());
  }

  return EXIT_SUCCESS;
}
