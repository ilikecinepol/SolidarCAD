#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <BRepGProp.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/ChamferToolSession.h"
#include "model/Document.h"
#include "model/EdgeFeatureLimits.h"
#include "model/ExtrudeFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition))                                                        \
      throw TestFailure(std::string(__FILE__) + ":" +                       \
                        std::to_string(__LINE__) + ": " #condition);         \
  } while (false)

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

std::optional<std::size_t> chamferableEdge(const TopoDS_Shape& shape,
                                           double distance) {
  for (std::size_t index = 0; index < 64; ++index) {
    std::string error;
    if (solidar::buildChamferShape(shape, {index}, distance, &error))
      return index;
    if (error == "Chamfer edge could not be resolved") break;
  }
  return std::nullopt;
}

struct Box {
  solidar::BodyId bodyId{};
  solidar::SketchId sketchId{};
  solidar::FeatureId extrudeId{};
};

Box addBox(solidar::Document& document) {
  auto& sketch = document.addSketch("Chamfer profile");
  sketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  auto& body = document.addBody("Chamfer body");
  auto& extrude = body.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(sketch.id, 50.0, "Extrude"));
  return {body.id(), sketch.id, extrude.id()};
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    QCoreApplication application(argc, argv);
    solidar::Document document;
    const Box box = addBox(document);
    CHECK(document.recompute());
    auto* body = document.findBody(box.bodyId);
    CHECK(body && body->resultShape());
    const auto sourceShape = body->resultShape();
    const auto firstIndex = chamferableEdge(*sourceShape, 2.0);
    CHECK(firstIndex);
    const auto firstReference = solidar::makeEdgeReference(
        *sourceShape, box.bodyId, box.extrudeId, *firstIndex);
    CHECK(firstReference.signature);

    // Single-edge feature, real B-Rep change, and parameter editing.
    const double baseVolume = volumeOf(*sourceShape);
    auto feature = std::make_unique<solidar::ChamferFeature>(
        firstReference, 2.0, "Chamfer");
    auto* chamfer = feature.get();
    const auto chamferId = chamfer->id();
    body->addFeature(std::move(feature));
    CHECK(document.recompute());
    CHECK(chamfer->isValid() && chamfer->hasShape());
    CHECK(std::abs(volumeOf(*chamfer->shape()) - baseVolume) > 1e-5);
    const double volumeAtTwo = volumeOf(*chamfer->shape());
    chamfer->setDistanceMm(1.0);
    CHECK(chamfer->isDirty());
    CHECK(document.recompute());
    CHECK(std::abs(volumeOf(*chamfer->shape()) - volumeAtTwo) > 1e-5);

    // Failure never exposes stale geometry and a valid edit recovers it.
    chamfer->setDistanceMm(1000.0);
    CHECK(!document.recompute());
    CHECK(chamfer->isFailed());
    CHECK(!chamfer->hasShape());
    CHECK(!chamfer->error().empty());
    chamfer->setDistanceMm(1.5);
    CHECK(document.recompute());
    CHECK(chamfer->isValid() && chamfer->hasShape());

    // Upstream geometry edits keep the persistent reference resolvable.
    auto* extrude =
        dynamic_cast<solidar::ExtrudeFeature*>(body->features().front().get());
    CHECK(extrude);
    extrude->setLengthMm(65.0);
    body->markDirtyFrom(0);
    CHECK(document.recompute());
    CHECK(chamfer->id() == chamferId && chamfer->isValid());

    // Preview, panel/manipulator synchronization, and Cancel are non-mutating.
    const auto upstream = body->features().front()->shape();
    CHECK(upstream);
    solidar::ChamferToolSession session;
    session.begin(box.bodyId, box.extrudeId, upstream, {firstReference}, 1.0);
    CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(session.previewShape());
    CHECK(session.manipulator());
    session.setDistanceFromPanel(1.25);
    CHECK(session.distanceMm() == 1.25);
    session.setDistanceFromManipulator(0.75);
    CHECK(session.distanceMm() == 0.75);
    session.cancel();
    CHECK(session.lifecycle() == solidar::ToolLifecycle::Inactive);
    CHECK(body->features().size() == 2);

    // Multiple edges are supported when OCCT accepts the pair.
    std::vector<solidar::EdgeReference> pair;
    for (std::size_t index = 0; index < 64 && pair.empty(); ++index) {
      for (std::size_t other = index + 1; other < 64; ++other) {
        std::string error;
        if (!solidar::buildChamferShape(*upstream, {index, other}, 1.0, &error)) {
          if (error == "Chamfer edge could not be resolved") break;
          continue;
        }
        pair = {solidar::makeEdgeReference(*upstream, box.bodyId, box.extrudeId,
                                           index),
                solidar::makeEdgeReference(*upstream, box.bodyId, box.extrudeId,
                                           other)};
        break;
      }
    }
    CHECK(pair.size() == 2);
    solidar::ChamferToolSession pairSession;
    pairSession.begin(box.bodyId, box.extrudeId, upstream, pair, 1.0);
    CHECK(pairSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(pairSession.previewShape());
    const auto pairReferences = pairSession.edges();
    pairSession.setDistanceFromPanel(1.25);
    CHECK(pairSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(pairSession.edges() == pairReferences);
    pairSession.setDistanceFromManipulator(0.75);
    CHECK(pairSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(pairSession.edges() == pairReferences);
    pairSession.setDistanceFromPanel(1000.0);
    CHECK(pairSession.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(pairSession.previewShape());
    CHECK(std::abs(pairSession.distanceMm() - 0.75) < 1e-9);
    CHECK(!pairSession.error().empty());
    CHECK(pairSession.maximumValidDistanceMm());
    CHECK(*pairSession.maximumValidDistanceMm() >= 0.75);
    CHECK(pairSession.edges() == pairReferences);
    pairSession.setDistanceFromManipulator(0.0);
    CHECK(pairSession.lifecycle() == solidar::ToolLifecycle::EditingParameters);
    CHECK(pairSession.previewShape().get() == upstream.get());
    pairSession.setDistanceFromPanel(1.0);
    CHECK(pairSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(pairSession.edges() == pairReferences);
    pairSession.setDistanceFromManipulator(1000.0);
    CHECK(pairSession.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(pairSession.previewShape());
    CHECK(pairSession.limitReached());
    CHECK(pairSession.maximumValidDistanceMm());
    CHECK(std::abs(pairSession.distanceMm() -
                   *pairSession.maximumValidDistanceMm()) < 1e-4);
    CHECK(pairSession.distanceMm() < 1000.0);

    // A hollow body's oversized chamfer can remain a formally valid B-Rep
    // while crossing into unrelated inner faces. Treat the nearest such face
    // as the geometric boundary instead of exposing the parasitic flange.
    const TopoDS_Shape outer = BRepPrimAPI_MakeBox(80.0, 50.0, 20.0).Shape();
    const TopoDS_Shape inner =
        BRepPrimAPI_MakeBox(gp_Pnt(4.0, 4.0, 4.0), 72.0, 42.0, 20.0).Shape();
    BRepAlgoAPI_Cut trayMaker(outer, inner);
    trayMaker.Build();
    CHECK(trayMaker.IsDone());
    const TopoDS_Shape tray = trayMaker.Shape();
    bool rejectedBoundaryCrossing = false;
    bool acceptedLocalChamfer = false;
    for (std::size_t index = 0; index < 64; ++index) {
      std::string smallError;
      if (!solidar::buildChamferShape(tray, {index}, 1.0, &smallError)) {
        if (smallError.find("could not be resolved") != std::string::npos) break;
        continue;
      }
      acceptedLocalChamfer = true;
      std::string largeError;
      const auto large =
          solidar::buildChamferShape(tray, {index}, 14.08, &largeError);
      if (!large &&
          largeError == "Chamfer exceeds the source shape boundary") {
        rejectedBoundaryCrossing = true;
        break;
      }
    }
    CHECK(acceptedLocalChamfer);
    CHECK(rejectedBoundaryCrossing);

    // Faces belonging to another selected edge are part of the same changing
    // region, not fixed obstacles. A paired inner/outer selection therefore
    // gets a wider domain than either edge would receive in isolation.
    struct TrayEdge {
      std::size_t index{};
      TopoDS_Edge edge;
      double individualClearance{};
    };
    std::vector<TrayEdge> trayEdges;
    for (std::size_t index = 0; index < 64; ++index) {
      const auto edge = solidar::resolveEdge(tray, index);
      if (!edge) break;
      std::string error;
      if (!solidar::buildChamferShape(tray, {index}, 1.0, &error)) continue;
      const auto clearance =
          solidar::minimumEdgeFeatureClearance(tray, {*edge});
      if (clearance) trayEdges.push_back({index, *edge, *clearance});
    }
    bool pairedSelectionRelaxesBoundary = false;
    for (std::size_t first = 0;
         first < trayEdges.size() && !pairedSelectionRelaxesBoundary; ++first) {
      for (std::size_t second = first + 1; second < trayEdges.size(); ++second) {
        const auto groupClearance = solidar::minimumEdgeFeatureClearance(
            tray, {trayEdges[first].edge, trayEdges[second].edge});
        const double oldClearance =
            std::min(trayEdges[first].individualClearance,
                     trayEdges[second].individualClearance);
        if (!groupClearance || *groupClearance <= oldClearance + 0.2) continue;
        const double candidate =
            oldClearance + std::min(0.5, (*groupClearance - oldClearance) * 0.5);
        std::string pairError;
        if (solidar::buildChamferShape(
                tray, {trayEdges[first].index, trayEdges[second].index},
                candidate, &pairError)) {
          pairedSelectionRelaxesBoundary = true;
          break;
        }
      }
    }
    CHECK(pairedSelectionRelaxesBoundary);

    // Editing an existing multi-edge feature restores its source references
    // while the preview remains an independent session result.
    solidar::ChamferFeature editableFeature(pair, 1.0, "Editable chamfer");
    solidar::ChamferToolSession editSession;
    editSession.begin(box.bodyId, box.extrudeId, upstream,
                      editableFeature.edges(), editableFeature.distanceMm(),
                      editableFeature.id());
    CHECK(editSession.editingFeatureId() == editableFeature.id());
    CHECK(editSession.edges() == editableFeature.edges());
    editSession.setEdges(pairReferences);
    CHECK(editSession.edges() == pairReferences);

    // Project v2 round-trip preserves ids, references, parameter, and rebuild.
    QTemporaryDir directory(QDir::current().filePath("chamfer-v1-XXXXXX"));
    CHECK(directory.isValid());
    const QString path = directory.filePath("round-trip.solidar");
    QString error;
    const bool saved =
        solidar::project::ProjectFile::saveDocument(path, document, &error);
    if (!saved)
      std::cerr << "save failed: " << error.toStdString() << '\n';
    CHECK(saved);
    solidar::Document restored;
    CHECK(solidar::project::ProjectFile::loadDocument(path, &restored, &error));
    CHECK(restored.recompute());
    const auto* restoredChamfer = dynamic_cast<const solidar::ChamferFeature*>(
        restored.findBody(box.bodyId)->activeFeature());
    CHECK(restoredChamfer);
    CHECK(restoredChamfer->id() == chamferId);
    CHECK(restoredChamfer->distanceMm() == 1.5);
    CHECK(restoredChamfer->edge().signature);

    // An intentionally corrupted persistent reference preserves resolver detail.
    solidar::Document broken;
    const Box brokenBox = addBox(broken);
    CHECK(broken.recompute());
    auto* brokenBody = broken.findBody(brokenBox.bodyId);
    const auto brokenIndex = chamferableEdge(*brokenBody->resultShape(), 1.0);
    CHECK(brokenIndex);
    auto badReference = solidar::makeEdgeReference(
        *brokenBody->resultShape(), brokenBox.bodyId, brokenBox.extrudeId,
        *brokenIndex);
    badReference.persistentTag = "edge:missing";
    badReference.edgeIndex = 999999;
    badReference.signature->midpoint.x += 100000.0;
    auto& bad = brokenBody->addFeature(
        std::make_unique<solidar::ChamferFeature>(badReference, 1.0));
    CHECK(!broken.recompute());
    CHECK(bad.isFailed() && !bad.hasShape());
    CHECK(bad.error().find("Chamfer edge could not be resolved: ") == 0);
    CHECK(bad.error().size() > std::string("Chamfer edge could not be resolved: ").size());
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
